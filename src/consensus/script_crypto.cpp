#include "consensus/script_interpreter.h"
#include "crypto/sha256.h"
#include "crypto/ripemd160.h"
#include <openssl/evp.h>
#include <cstring>
#include <stdexcept>

namespace dinero {
namespace consensus {
namespace {

std::vector<uint8_t> ComputeSha1(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(20);
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new failed for SHA1");
    }

    const bool ok =
        EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1 &&
        (data.empty() || EVP_DigestUpdate(ctx, data.data(), data.size()) == 1) &&
        EVP_DigestFinal_ex(ctx, hash.data(), &out_len) == 1 &&
        out_len == hash.size();

    EVP_MD_CTX_free(ctx);

    if (!ok) {
        throw std::runtime_error("EVP SHA1 digest failed");
    }
    return hash;
}

} // namespace

// ============================================================================
// Phase 24.4: Cryptographic Primitives for Script Opcodes
// ============================================================================

/**
 * SHA256 - Single SHA256 hash
 */
std::vector<uint8_t> SHA256_Hash(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(32);
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash.data());
    return hash;
}

/**
 * HASH256 - Double SHA256 (SHA256(SHA256(x)))
 * Note: This returns bytes in natural order (big-endian), not reversed like Bitcoin txids
 */
std::vector<uint8_t> HASH256_Hash(const std::vector<uint8_t>& data) {
    // First SHA256
    std::vector<uint8_t> hash1 = SHA256_Hash(data);
    // Second SHA256
    return SHA256_Hash(hash1);
}

/**
 * RIPEMD160 - RIPEMD160 hash
 */
std::vector<uint8_t> RIPEMD160_Hash(const std::vector<uint8_t>& data) {
    const auto hash = dinero::RIPEMD160(data.data(), data.size());
    return std::vector<uint8_t>(hash.begin(), hash.end());
}

/**
 * HASH160 - RIPEMD160(SHA256(x))
 *
 * This is the standard Bitcoin address hash used in P2PKH.
 */
std::vector<uint8_t> HASH160_Hash(const std::vector<uint8_t>& data) {
    // First SHA256
    std::vector<uint8_t> sha256_hash = SHA256_Hash(data);

    // Then RIPEMD160
    return RIPEMD160_Hash(sha256_hash);
}

/**
 * SHA1 - SHA1 hash (legacy, not recommended)
 */
std::vector<uint8_t> SHA1_Hash(const std::vector<uint8_t>& data) {
    return ComputeSha1(data);
}

} // namespace consensus
} // namespace dinero
