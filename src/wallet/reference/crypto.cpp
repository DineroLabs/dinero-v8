#include "crypto.h"
#include "wallet/bip39.h"
#include "crypto/hd_keychain.h"
#include "crypto/dinero_crypto_minimal.h"
#include "crypto/base58.hpp"
#include "external/bech32/bech32.hpp"
#include <secp256k1.h>
#include <openssl/crypto.h>  // OPENSSL_cleanse for memory zeroization
#include <stdexcept>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace wallet {
namespace reference {
namespace crypto {

// Global secp256k1 context
static secp256k1_context* GetSecp256k1Context() {
    static secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
    );
    return ctx;
}

// BIP39 Implementation
std::string BIP39::GenerateMnemonic(int word_count) {
    // Map word count to dinero::bip39::WordCount enum
    dinero::bip39::WordCount wc;
    switch (word_count) {
        case 12: wc = dinero::bip39::WordCount::Words12; break;
        case 15: wc = dinero::bip39::WordCount::Words15; break;
        case 18: wc = dinero::bip39::WordCount::Words18; break;
        case 21: wc = dinero::bip39::WordCount::Words21; break;
        case 24: wc = dinero::bip39::WordCount::Words24; break;
        default:
            throw std::invalid_argument("Invalid word count. Must be 12, 15, 18, 21, or 24");
    }

    return dinero::bip39::Generate(wc);
}

bool BIP39::ValidateMnemonic(const std::string& mnemonic) {
    return dinero::bip39::ValidateMnemonic(mnemonic);
}

std::vector<uint8_t> BIP39::MnemonicToSeed(const std::string& mnemonic, const std::string& passphrase) {
    std::vector<uint8_t> seed;
    if (!dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed)) {
        throw std::runtime_error("Failed to convert mnemonic to seed");
    }
    return seed;
}

// BIP32 Implementation
BIP32::ExtendedKey BIP32::MasterKeyFromSeed(const std::vector<uint8_t>& seed) {
    auto master = dinero::crypto::HDKeychain::fromSeed(seed);

    ExtendedKey result;
    result.chain_code = std::vector<uint8_t>(master.chain_code.begin(), master.chain_code.end());
    result.private_key = std::vector<uint8_t>(master.private_key.begin(), master.private_key.end());
    result.public_key = std::vector<uint8_t>(master.public_key.begin(), master.public_key.end());

    // SECURITY: Zeroize intermediate key material
    OPENSSL_cleanse(master.private_key.data(), master.private_key.size());
    OPENSSL_cleanse(master.chain_code.data(), master.chain_code.size());

    return result;
}

BIP32::ExtendedKey BIP32::DeriveChild(const ExtendedKey& parent, uint32_t index) {
    // Convert to dinero::crypto::HDKeychain::ExtendedKey
    dinero::crypto::HDKeychain::ExtendedKey parent_key;
    std::copy(parent.chain_code.begin(), parent.chain_code.end(), parent_key.chain_code.begin());
    std::copy(parent.private_key.begin(), parent.private_key.end(), parent_key.private_key.begin());
    std::copy(parent.public_key.begin(), parent.public_key.end(), parent_key.public_key.begin());

    auto child = parent_key.derive(index);

    ExtendedKey result;
    result.chain_code = std::vector<uint8_t>(child.chain_code.begin(), child.chain_code.end());
    result.private_key = std::vector<uint8_t>(child.private_key.begin(), child.private_key.end());
    result.public_key = std::vector<uint8_t>(child.public_key.begin(), child.public_key.end());

    // SECURITY: Zeroize intermediate key material
    OPENSSL_cleanse(parent_key.private_key.data(), parent_key.private_key.size());
    OPENSSL_cleanse(parent_key.chain_code.data(), parent_key.chain_code.size());
    OPENSSL_cleanse(child.private_key.data(), child.private_key.size());
    OPENSSL_cleanse(child.chain_code.data(), child.chain_code.size());

    return result;
}

BIP32::ExtendedKey BIP32::DerivePath(const ExtendedKey& master, const std::string& path) {
    // Parse path like "m/84'/1448'/0'/0/0"
    if (path.empty() || path[0] != 'm') {
        throw std::invalid_argument("Path must start with 'm'");
    }

    ExtendedKey current = master;

    // Split path by '/' and process each component
    std::string remaining = path.substr(1); // Remove 'm'

    while (!remaining.empty()) {
        // Skip leading '/'
        if (remaining[0] == '/') {
            remaining = remaining.substr(1);
        }

        if (remaining.empty()) break;

        // Find next '/'
        size_t pos = remaining.find('/');
        std::string segment = (pos != std::string::npos) ? remaining.substr(0, pos) : remaining;

        // Check for hardened derivation (ends with ')
        bool hardened = false;
        if (!segment.empty() && segment.back() == '\'') {
            hardened = true;
            segment.pop_back(); // Remove the apostrophe
        }

        if (segment.empty()) {
            throw std::invalid_argument("Invalid path component");
        }

        // Parse index
        uint32_t index = std::stoul(segment);
        if (hardened) {
            index |= 0x80000000;  // Set hardened bit
        }

        // Derive child
        current = DeriveChild(current, index);

        // Move to next segment
        if (pos != std::string::npos) {
            remaining = remaining.substr(pos);
        } else {
            break;
        }
    }

    return current;
}

// ECC Implementation
std::vector<uint8_t> ECC::GeneratePrivateKey() {
    std::vector<uint8_t> private_key(32);
    auto ctx = GetSecp256k1Context();

    // Generate random private key
    do {
        if (!CF_GenerateRandomBytes(private_key.data(), 32)) {
            throw std::runtime_error("Failed to generate random bytes");
        }
    } while (!secp256k1_ec_seckey_verify(ctx, private_key.data()));

    return private_key;
}

std::vector<uint8_t> ECC::DerivePublicKey(const std::vector<uint8_t>& private_key, bool compressed) {
    if (private_key.size() != 32) {
        throw std::invalid_argument("Private key must be 32 bytes");
    }

    auto ctx = GetSecp256k1Context();
    secp256k1_pubkey pubkey;

    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, private_key.data())) {
        throw std::runtime_error("Failed to create public key");
    }

    std::vector<uint8_t> result(compressed ? 33 : 65);
    size_t len = result.size();
    secp256k1_ec_pubkey_serialize(
        ctx,
        result.data(),
        &len,
        &pubkey,
        compressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED
    );

    return result;
}

std::vector<uint8_t> ECC::Sign(const std::vector<uint8_t>& private_key, const std::vector<uint8_t>& message_hash) {
    if (private_key.size() != 32 || message_hash.size() != 32) {
        throw std::invalid_argument("Private key and message hash must be 32 bytes");
    }

    auto ctx = GetSecp256k1Context();
    secp256k1_ecdsa_signature sig;

    if (!secp256k1_ecdsa_sign(ctx, &sig, message_hash.data(), private_key.data(), nullptr, nullptr)) {
        throw std::runtime_error("Failed to sign message");
    }

    std::vector<uint8_t> result(64);
    secp256k1_ecdsa_signature_serialize_compact(ctx, result.data(), &sig);

    return result;
}

bool ECC::Verify(const std::vector<uint8_t>& public_key, const std::vector<uint8_t>& message_hash, const std::vector<uint8_t>& signature) {
    if (message_hash.size() != 32 || signature.size() != 64) {
        return false;
    }

    auto ctx = GetSecp256k1Context();

    // Parse public key
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, public_key.data(), public_key.size())) {
        return false;
    }

    // Parse signature
    secp256k1_ecdsa_signature sig;
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &sig, signature.data())) {
        return false;
    }

    return secp256k1_ecdsa_verify(ctx, &sig, message_hash.data(), &pubkey) == 1;
}

// Address Implementation
std::string Address::Encode(const std::vector<uint8_t>& public_key_hash, const std::string& hrp, int witness_version) {
    // Encode using bech32::Encode
    return bech32::Encode(hrp, witness_version, public_key_hash);
}

std::vector<uint8_t> Address::Decode(const std::string& address) {
    auto result = bech32::Decode("din", address);
    if (!result.has_value()) {
        throw std::invalid_argument("Invalid bech32 address");
    }
    return result->program;
}

bool Address::Validate(const std::string& address, const std::string& hrp) {
    auto result = bech32::Decode(hrp, address);
    return result.has_value();
}

std::string Address::PublicKeyToAddress(const std::vector<uint8_t>& public_key) {
    if (public_key.size() != 33) {
        throw std::invalid_argument("Public key must be 33 bytes (compressed)");
    }

    // Hash160 = RIPEMD160(SHA256(pubkey))
    auto hash160 = Hash::Hash160(public_key);

    // Encode as bech32
    return Encode(hash160, "din", 0);
}

// Hash Implementation
std::vector<uint8_t> Hash::SHA256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(32);
    sha256(data.data(), data.size(), hash.data());
    return hash;
}

std::vector<uint8_t> Hash::Hash256(const std::vector<uint8_t>& data) {
    auto hash1 = SHA256(data);
    return SHA256(hash1);
}

std::vector<uint8_t> Hash::RIPEMD160(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(20);
    ripemd160(data.data(), data.size(), hash.data());
    return hash;
}

std::vector<uint8_t> Hash::Hash160(const std::vector<uint8_t>& data) {
    auto sha = SHA256(data);
    return RIPEMD160(sha);
}

std::vector<uint8_t> Hash::HMAC_SHA512(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> result(64);
    hmac_sha512(key.data(), key.size(), data.data(), data.size(), result.data());
    return result;
}

// WIF Implementation
std::string WIF::Encode(const std::vector<uint8_t>& private_key, bool compressed, uint8_t version) {
    if (private_key.size() != 32) {
        throw std::invalid_argument("Private key must be 32 bytes");
    }

    std::vector<uint8_t> data;
    data.push_back(version);
    data.insert(data.end(), private_key.begin(), private_key.end());

    if (compressed) {
        data.push_back(0x01);
    }

    // Use dinero::b58::encode_check
    std::string result = dinero::b58::encode_check(data.data(), data.size());

    // SECURITY: Zeroize intermediate buffer containing private key
    OPENSSL_cleanse(data.data(), data.size());

    return result;
}

std::vector<uint8_t> WIF::Decode(const std::string& wif) {
    std::vector<uint8_t> decoded;
    if (!dinero::b58::decode_check(wif, decoded)) {
        throw std::invalid_argument("Invalid WIF format");
    }

    if (decoded.size() < 33) {
        // SECURITY: Zeroize even on error path
        OPENSSL_cleanse(decoded.data(), decoded.size());
        throw std::invalid_argument("Invalid WIF format");
    }

    // Extract private key (skip version byte, take 32 bytes)
    std::vector<uint8_t> result(decoded.begin() + 1, decoded.begin() + 33);

    // SECURITY: Zeroize decoded buffer containing private key
    OPENSSL_cleanse(decoded.data(), decoded.size());

    return result;
}

} // namespace crypto
} // namespace reference
} // namespace wallet
} // namespace dinero
