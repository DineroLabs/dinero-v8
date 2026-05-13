#include "crypto/hd_keychain.h"
#include "crypto/bip39.hpp"
#include "crypto/bip32_slip132.hpp"
#include "crypto/dinero_crypto_minimal.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/tagged_hash.h"
#include "external/bech32/bech32.hpp"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <openssl/crypto.h>  // OPENSSL_cleanse for secure zeroization
#include <cstring>
#include <cassert>
#include <stdexcept>
namespace dinero {
namespace crypto {

secp256k1_context* HDKeychain::getContext() {
    return GetSecp256k1ContextSignVerify();
}

void HDKeychain::hmacSha512(const uint8_t* key, size_t key_len, 
                           const uint8_t* data, size_t data_len, 
                           uint8_t* out) {
    ::hmac_sha512(key, key_len, data, data_len, out);
}

HDKeychain::ExtendedKey::ExtendedKey() 
    : fingerprint(0), depth(0), child_number(0), is_private(true) {
    chain_code.fill(0);
    private_key.fill(0);
    public_key.fill(0);
}

HDKeychain::ExtendedKey::~ExtendedKey() {
    // SECURITY: Use OPENSSL_cleanse to prevent compiler optimization
    // .fill(0) can be optimized away by compilers
    OPENSSL_cleanse(private_key.data(), private_key.size());
    OPENSSL_cleanse(chain_code.data(), chain_code.size());
}

HDKeychain::ExtendedKey HDKeychain::ExtendedKey::derive(uint32_t index) const {
    ExtendedKey child;
    child.depth = depth + 1;
    child.child_number = index;
    child.is_private = is_private;
    
    // Compute fingerprint from parent public key
    uint8_t hash[20];
    HASH160(public_key.data(), PUBKEY_SIZE, hash);
    child.fingerprint = (hash[0] << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
    
    // Prepare data for HMAC
    uint8_t data[37];
    bool hardened = (index >= HARDENED_KEY_START);
    
    if (hardened) {
        // Hardened derivation: ser256(kpar) || ser32(i)
        data[0] = 0x00;
        memcpy(data + 1, private_key.data(), PRIVKEY_SIZE);
    } else {
        // Non-hardened derivation: serP(point(kpar)) || ser32(i)
        memcpy(data, public_key.data(), PUBKEY_SIZE);
    }
    
    // Serialize index in big-endian
    data[33] = (index >> 24) & 0xFF;
    data[34] = (index >> 16) & 0xFF;
    data[35] = (index >> 8) & 0xFF;
    data[36] = index & 0xFF;
    
    // HMAC-SHA512
    uint8_t hmac_result[64];
    hmacSha512(chain_code.data(), CHAINCODE_SIZE, data, hardened ? 37 : 37, hmac_result);
    
    // Split result
    memcpy(child.private_key.data(), hmac_result, PRIVKEY_SIZE);
    memcpy(child.chain_code.data(), hmac_result + 32, CHAINCODE_SIZE);
    
    // Add parent private key to child private key (mod n)
    secp256k1_context* ctx = getContext();
    if (!secp256k1_ec_seckey_tweak_add(ctx, child.private_key.data(), private_key.data())) {
        // Invalid key, should retry with next index in practice
        throw std::runtime_error("Invalid child key derived");
    }
    
    // Derive public key from private key
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, child.private_key.data())) {
        throw std::runtime_error("Failed to create public key");
    }
    
    size_t pubkey_len = PUBKEY_SIZE;
    secp256k1_ec_pubkey_serialize(ctx, child.public_key.data(), &pubkey_len, 
                                 &pubkey, SECP256K1_EC_COMPRESSED);
    
    return child;
}

std::array<uint8_t, HDKeychain::PUBKEY_SIZE> HDKeychain::ExtendedKey::getPublicKey() const {
    return public_key;
}

std::string HDKeychain::ExtendedKey::getAddress(const std::string& hrp) const {
    auto hash160 = getHash160();

    // Use proper bech32 encoding with witness version 0 (P2WPKH)
    std::vector<uint8_t> program(hash160.begin(), hash160.end());
    return bech32::Encode(hrp, 0, program);
}

std::array<uint8_t, HDKeychain::XONLY_SIZE> HDKeychain::ExtendedKey::getXOnlyPubkey() const {
    // x-only = compressed pubkey bytes [1..32] (drop the 0x02/0x03 sign byte)
    std::array<uint8_t, XONLY_SIZE> xonly{};
    std::memcpy(xonly.data(), public_key.data() + 1, XONLY_SIZE);
    return xonly;
}

std::string HDKeychain::ExtendedKey::getTaprootAddress(const std::string& hrp) const {
    // 1. Get x-only pubkey (internal key)
    auto xonly = getXOnlyPubkey();
    auto* ctx = getContext();

    // 2. Parse x-only pubkey
    secp256k1_xonly_pubkey internal_pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &internal_pubkey, xonly.data())) {
        throw std::runtime_error("Failed to parse x-only pubkey for Taproot address");
    }

    // 3. BIP86 TapTweak: tagged_hash("TapTweak", internal_key)
    uint8_t tweak[32];
    dinero::crypto::TaggedHash("TapTweak", xonly.data(), XONLY_SIZE, tweak);

    // 4. Apply tweak: output = internal + tweak * G
    secp256k1_pubkey output_pubkey;
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pubkey, &internal_pubkey, tweak)) {
        throw std::runtime_error("Failed to apply BIP341 tweak for Taproot address");
    }

    // 5. Convert to x-only (drop Y coordinate)
    secp256k1_xonly_pubkey output_xonly;
    int parity;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_xonly, &parity, &output_pubkey)) {
        throw std::runtime_error("Failed to convert tweaked pubkey to x-only");
    }

    // 6. Serialize tweaked x-only pubkey
    std::array<uint8_t, 32> tweaked_key{};
    if (!secp256k1_xonly_pubkey_serialize(ctx, tweaked_key.data(), &output_xonly)) {
        throw std::runtime_error("Failed to serialize tweaked x-only pubkey");
    }

    // 7. Encode as bech32m witness version 1
    std::vector<uint8_t> program(tweaked_key.begin(), tweaked_key.end());
    return bech32::Encode(hrp, 1, program, bech32::Encoding::BECH32M);
}

std::array<uint8_t, HDKeychain::HASH160_SIZE> HDKeychain::ExtendedKey::getHash160() const {
    std::array<uint8_t, HASH160_SIZE> hash;
    HASH160(public_key.data(), PUBKEY_SIZE, hash.data());
    return hash;
}

std::string HDKeychain::ExtendedKey::serialize(bool mainnet) const {
    // Use existing BIP32 serialization from bip32_slip132
    dinero::bip32::NodeSer node = dinero::bip32::serialize_xpub(depth, fingerprint, child_number, 
                                                               chain_code.data(), public_key.data());
    if (mainnet) {
        return dinero::bip32::to_zpub_mainnet(node); // BIP84 uses zpub for mainnet
    } else {
        return dinero::bip32::to_vpub_testnet(node); // BIP84 uses vpub for testnet
    }
}

HDKeychain::ExtendedKey HDKeychain::fromSeed(const std::vector<uint8_t>& seed) {
    if (seed.size() < 16 || seed.size() > 64) {
        throw std::invalid_argument("Invalid seed length");
    }
    
    ExtendedKey master;
    master.depth = 0;
    master.child_number = 0;
    master.fingerprint = 0;
    master.is_private = true;
    
    // HMAC-SHA512("Bitcoin seed", seed)
    const char* hmac_key = "Bitcoin seed";
    uint8_t hmac_result[64];
    hmacSha512(reinterpret_cast<const uint8_t*>(hmac_key), strlen(hmac_key),
               seed.data(), seed.size(), hmac_result);
    
    // Split result: first 32 bytes = private key, last 32 bytes = chain code
    memcpy(master.private_key.data(), hmac_result, PRIVKEY_SIZE);
    memcpy(master.chain_code.data(), hmac_result + 32, CHAINCODE_SIZE);
    
    // Derive public key
    secp256k1_context* ctx = getContext();
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, master.private_key.data())) {
        throw std::runtime_error("Invalid master private key");
    }
    
    size_t pubkey_len = PUBKEY_SIZE;
    secp256k1_ec_pubkey_serialize(ctx, master.public_key.data(), &pubkey_len,
                                 &pubkey, SECP256K1_EC_COMPRESSED);
    
    return master;
}

HDKeychain::ExtendedKey HDKeychain::fromMnemonic(const std::string& mnemonic, 
                                                const std::string& passphrase) {
    uint8_t seed[64];
    dinero::bip39::mnemonic_to_seed(mnemonic, passphrase, seed);
    
    std::vector<uint8_t> seed_vec(seed, seed + 64);
    return fromSeed(seed_vec);
}

HDKeychain::ExtendedKey HDKeychain::deriveBIP84(const ExtendedKey& master, 
                                               uint32_t coin_type, 
                                               uint32_t account, 
                                               uint32_t change, 
                                               uint32_t index) {
    // m/84'/coin'/account'/change/index
    auto purpose_key = master.derive(BIP84_PURPOSE | HARDENED_KEY_START);
    auto coin_key = purpose_key.derive(coin_type | HARDENED_KEY_START);
    auto account_key = coin_key.derive(account | HARDENED_KEY_START);
    auto change_key = account_key.derive(change);
    return change_key.derive(index);
}

HDKeychain::ExtendedKey HDKeychain::getBIP84Account(const ExtendedKey& master, 
                                                   uint32_t coin_type, 
                                                   uint32_t account) {
    // m/84'/coin'/account'
    auto purpose_key = master.derive(BIP84_PURPOSE | HARDENED_KEY_START);
    auto coin_key = purpose_key.derive(coin_type | HARDENED_KEY_START);
    return coin_key.derive(account | HARDENED_KEY_START);
}

// BIP84AddressGenerator implementation
BIP84AddressGenerator::BIP84AddressGenerator(const HDKeychain::ExtendedKey& account_key,
                                           const std::string& hrp,
                                           uint32_t gap_limit)
    : account_key_(account_key), hrp_(hrp), gap_limit_(gap_limit), 
      external_index_(0), internal_index_(0) {
}

std::string BIP84AddressGenerator::getNextAddress() {
    auto address = getAddress(EXTERNAL_CHAIN, external_index_);
    external_index_++;
    return address;
}

std::string BIP84AddressGenerator::getNextChangeAddress() {
    auto address = getAddress(INTERNAL_CHAIN, internal_index_);
    internal_index_++;
    return address;
}

std::string BIP84AddressGenerator::getAddress(uint32_t chain, uint32_t index) {
    auto change_key = account_key_.derive(chain);
    auto address_key = change_key.derive(index);
    return address_key.getAddress(hrp_);
}

bool BIP84AddressGenerator::isOwnAddress(const std::string& address) const {
    // Check external chain
    for (uint32_t i = 0; i < external_index_ + gap_limit_; ++i) {
        auto change_key = account_key_.derive(EXTERNAL_CHAIN);
        auto addr_key = change_key.derive(i);
        if (addr_key.getAddress(hrp_) == address) {
            return true;
        }
    }
    
    // Check internal chain
    for (uint32_t i = 0; i < internal_index_ + gap_limit_; ++i) {
        auto change_key = account_key_.derive(INTERNAL_CHAIN);
        auto addr_key = change_key.derive(i);
        if (addr_key.getAddress(hrp_) == address) {
            return true;
        }
    }
    
    return false;
}

std::vector<std::string> BIP84AddressGenerator::scanAddresses(uint32_t chain, uint32_t start_index) {
    std::vector<std::string> addresses;
    auto change_key = account_key_.derive(chain);
    
    for (uint32_t i = start_index; i < start_index + gap_limit_; ++i) {
        auto addr_key = change_key.derive(i);
        addresses.push_back(addr_key.getAddress(hrp_));
    }
    
    return addresses;
}

// ═══════════════════════════════════════════════════════════════════════════
// BIP86 Taproot derivation (DEFAULT)
// ═══════════════════════════════════════════════════════════════════════════

HDKeychain::ExtendedKey HDKeychain::deriveBIP86(const ExtendedKey& master,
                                               uint32_t coin_type,
                                               uint32_t account,
                                               uint32_t change,
                                               uint32_t index) {
    // m/86'/coin'/account'/change/index
    auto purpose_key = master.derive(BIP86_PURPOSE | HARDENED_KEY_START);
    auto coin_key = purpose_key.derive(coin_type | HARDENED_KEY_START);
    auto account_key = coin_key.derive(account | HARDENED_KEY_START);
    auto change_key = account_key.derive(change);
    return change_key.derive(index);
}

HDKeychain::ExtendedKey HDKeychain::getBIP86Account(const ExtendedKey& master,
                                                   uint32_t coin_type,
                                                   uint32_t account) {
    // m/86'/coin'/account'
    auto purpose_key = master.derive(BIP86_PURPOSE | HARDENED_KEY_START);
    auto coin_key = purpose_key.derive(coin_type | HARDENED_KEY_START);
    return coin_key.derive(account | HARDENED_KEY_START);
}

// BIP86AddressGenerator implementation
BIP86AddressGenerator::BIP86AddressGenerator(const HDKeychain::ExtendedKey& account_key,
                                           const std::string& hrp,
                                           uint32_t gap_limit)
    : account_key_(account_key), hrp_(hrp), gap_limit_(gap_limit),
      external_index_(0), internal_index_(0) {
}

std::string BIP86AddressGenerator::getNextAddress() {
    auto address = getAddress(EXTERNAL_CHAIN, external_index_);
    external_index_++;
    return address;
}

std::string BIP86AddressGenerator::getNextChangeAddress() {
    auto address = getAddress(INTERNAL_CHAIN, internal_index_);
    internal_index_++;
    return address;
}

std::string BIP86AddressGenerator::getAddress(uint32_t chain, uint32_t index) {
    auto change_key = account_key_.derive(chain);
    auto address_key = change_key.derive(index);
    return address_key.getTaprootAddress(hrp_);
}

bool BIP86AddressGenerator::isOwnAddress(const std::string& address) const {
    for (uint32_t i = 0; i < external_index_ + gap_limit_; ++i) {
        auto change_key = account_key_.derive(EXTERNAL_CHAIN);
        auto addr_key = change_key.derive(i);
        if (addr_key.getTaprootAddress(hrp_) == address) {
            return true;
        }
    }
    for (uint32_t i = 0; i < internal_index_ + gap_limit_; ++i) {
        auto change_key = account_key_.derive(INTERNAL_CHAIN);
        auto addr_key = change_key.derive(i);
        if (addr_key.getTaprootAddress(hrp_) == address) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> BIP86AddressGenerator::scanAddresses(uint32_t chain, uint32_t start_index) {
    std::vector<std::string> addresses;
    auto change_key = account_key_.derive(chain);
    for (uint32_t i = start_index; i < start_index + gap_limit_; ++i) {
        auto addr_key = change_key.derive(i);
        addresses.push_back(addr_key.getTaprootAddress(hrp_));
    }
    return addresses;
}

} // namespace crypto
} // namespace dinero
