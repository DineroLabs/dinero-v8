// SPDX-License-Identifier: MIT
// Dinero - Extended Public Key (BIP32 xpub deserialization)

#include "crypto/extended_pubkey.h"
#include "crypto/base58.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/hash160.h"
#include <secp256k1.h>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace crypto {

secp256k1_context* ExtendedPubKey::GetContext() {
    return GetSecp256k1ContextSignVerify();
}

ExtendedPubKey ExtendedPubKey::FromString(const std::string& xpub_string) {
    // Decode base58check
    std::vector<uint8_t> decoded;
    if (!dinero::b58::decode_check(xpub_string, decoded)) {
        throw std::invalid_argument("Invalid base58check encoding");
    }

    // BIP32 extended key format: 78 bytes
    // version(4) || depth(1) || parent_fpr(4) || child_num(4) || chain_code(32) || key_data(33)
    if (decoded.size() != 78) {
        throw std::invalid_argument("Invalid extended key length (expected 78 bytes, got " +
                                   std::to_string(decoded.size()) + ")");
    }

    // Extract version (big-endian)
    uint32_t version = (decoded[0] << 24) | (decoded[1] << 16) |
                      (decoded[2] << 8) | decoded[3];

    // Validate version (must be public key version)
    if (version != VERSION_XPUB && version != VERSION_YPUB && version != VERSION_ZPUB) {
        std::ostringstream oss;
        oss << "Invalid version bytes: 0x" << std::hex << std::setw(8)
            << std::setfill('0') << version;
        oss << " (expected xpub/ypub/zpub)";
        throw std::invalid_argument(oss.str());
    }

    ExtendedPubKey key;

    // Extract fields
    key.depth_ = decoded[4];

    key.parent_fingerprint_ = (decoded[5] << 24) | (decoded[6] << 16) |
                             (decoded[7] << 8) | decoded[8];

    key.child_number_ = (decoded[9] << 24) | (decoded[10] << 16) |
                       (decoded[11] << 8) | decoded[12];

    // Chain code (32 bytes)
    std::memcpy(key.chain_code_.data(), &decoded[13], CHAINCODE_SIZE);

    // Public key (33 bytes)
    std::memcpy(key.public_key_.data(), &decoded[45], PUBKEY_SIZE);

    // Validate public key format
    if (key.public_key_[0] != 0x02 && key.public_key_[0] != 0x03) {
        throw std::invalid_argument("Invalid public key format (must be compressed)");
    }

    // Verify public key is valid on secp256k1 curve
    secp256k1_context* ctx = GetContext();
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, key.public_key_.data(), PUBKEY_SIZE)) {
        throw std::invalid_argument("Invalid public key (not on secp256k1 curve)");
    }

    return key;
}

ExtendedPubKey ExtendedPubKey::Derive(uint32_t index) const {
    // Hardened derivation not supported for public keys
    if (IsHardened(index)) {
        throw std::invalid_argument("Cannot derive hardened child from extended public key");
    }

    ExtendedPubKey child;
    child.depth_ = depth_ + 1;
    child.child_number_ = index;

    // Compute parent fingerprint from this key's public key
    uint8_t hash[HASH160_SIZE];
    HASH160(public_key_.data(), PUBKEY_SIZE, hash);
    child.parent_fingerprint_ = (hash[0] << 24) | (hash[1] << 16) |
                                (hash[2] << 8) | hash[3];

    // BIP32 public key derivation:
    // CKDpub((Kpar, cpar), i) → (Ki, ci)
    //   hash = HMAC-SHA512(cpar, serP(Kpar) || ser32(i))
    //   Ki = point(parse256(hash_L)) + Kpar
    //   ci = hash_R

    // Prepare data: serP(Kpar) || ser32(i)
    uint8_t data[37];
    std::memcpy(data, public_key_.data(), PUBKEY_SIZE);
    data[33] = (index >> 24) & 0xFF;
    data[34] = (index >> 16) & 0xFF;
    data[35] = (index >> 8) & 0xFF;
    data[36] = index & 0xFF;

    // HMAC-SHA512(chain_code, data)
    uint8_t hmac_result[64];
    ::hmac_sha512(chain_code_.data(), CHAINCODE_SIZE, data, 37, hmac_result);

    // Split result: hash_L (32 bytes) and hash_R (32 bytes)
    // ci = hash_R (new chain code)
    std::memcpy(child.chain_code_.data(), &hmac_result[32], CHAINCODE_SIZE);

    // Ki = point(hash_L) + Kpar
    secp256k1_context* ctx = GetContext();

    // Parse parent public key
    secp256k1_pubkey parent_pubkey;
    if (!secp256k1_ec_pubkey_parse(ctx, &parent_pubkey, public_key_.data(), PUBKEY_SIZE)) {
        throw std::runtime_error("Failed to parse parent public key");
    }

    // Add tweak (hash_L) to parent pubkey
    // This computes point(hash_L) + Kpar
    if (!secp256k1_ec_pubkey_tweak_add(ctx, &parent_pubkey, hmac_result)) {
        throw std::runtime_error("Child key derivation failed (point at infinity)");
    }

    // Serialize child public key (compressed)
    size_t pubkey_len = PUBKEY_SIZE;
    if (!secp256k1_ec_pubkey_serialize(ctx, child.public_key_.data(), &pubkey_len,
                                       &parent_pubkey, SECP256K1_EC_COMPRESSED)) {
        throw std::runtime_error("Failed to serialize child public key");
    }

    return child;
}

std::vector<uint8_t> ExtendedPubKey::GetPublicKey() const {
    return std::vector<uint8_t>(public_key_.begin(), public_key_.end());
}

std::vector<uint8_t> ExtendedPubKey::GetChainCode() const {
    return std::vector<uint8_t>(chain_code_.begin(), chain_code_.end());
}

uint32_t ExtendedPubKey::GetFingerprint() const {
    uint8_t hash[HASH160_SIZE];
    HASH160(public_key_.data(), PUBKEY_SIZE, hash);
    return (hash[0] << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
}

std::string ExtendedPubKey::Serialize(uint32_t version) const {
    // BIP32 extended key format: 78 bytes
    std::vector<uint8_t> data(78);

    // Version (4 bytes, big-endian)
    data[0] = (version >> 24) & 0xFF;
    data[1] = (version >> 16) & 0xFF;
    data[2] = (version >> 8) & 0xFF;
    data[3] = version & 0xFF;

    // Depth (1 byte)
    data[4] = depth_;

    // Parent fingerprint (4 bytes, big-endian)
    data[5] = (parent_fingerprint_ >> 24) & 0xFF;
    data[6] = (parent_fingerprint_ >> 16) & 0xFF;
    data[7] = (parent_fingerprint_ >> 8) & 0xFF;
    data[8] = parent_fingerprint_ & 0xFF;

    // Child number (4 bytes, big-endian)
    data[9] = (child_number_ >> 24) & 0xFF;
    data[10] = (child_number_ >> 16) & 0xFF;
    data[11] = (child_number_ >> 8) & 0xFF;
    data[12] = child_number_ & 0xFF;

    // Chain code (32 bytes)
    std::memcpy(&data[13], chain_code_.data(), CHAINCODE_SIZE);

    // Public key (33 bytes)
    std::memcpy(&data[45], public_key_.data(), PUBKEY_SIZE);

    // Base58check encode
    return dinero::b58::encode_check(data.data(), data.size());
}

} // namespace crypto
} // namespace dinero
