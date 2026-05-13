#ifndef DINERO_CRYPTO_TAGGED_HASH_H
#define DINERO_CRYPTO_TAGGED_HASH_H

// Canonical BIP340 TaggedHash implementation.
// All Taproot-related tagged hashes (TapTweak, TapLeaf, TapBranch, BIP341 sighash)
// MUST use this single implementation. Do NOT hand-roll TaggedHash elsewhere.

#include "crypto/sha256.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dinero {
namespace crypto {

// BIP340 TaggedHash: SHA256(SHA256(tag) || SHA256(tag) || data)
inline void TaggedHash(const char* tag, const uint8_t* data, size_t len, uint8_t out[32]) {
    uint8_t tag_hash[32];
    CSHA256().Write(reinterpret_cast<const uint8_t*>(tag), std::strlen(tag)).Finalize(tag_hash);
    CSHA256()
        .Write(tag_hash, 32)
        .Write(tag_hash, 32)
        .Write(data, len)
        .Finalize(out);
}

// Convenience overload: vector in, vector out
inline std::vector<uint8_t> TaggedHash(const std::string& tag, const std::vector<uint8_t>& data) {
    uint8_t result[32];
    TaggedHash(tag.c_str(), data.data(), data.size(), result);
    return std::vector<uint8_t>(result, result + 32);
}

// Convenience overload: raw pointer in, std::array out
inline std::array<uint8_t, 32> TaggedHashArray(const std::string& tag, const uint8_t* data, size_t len) {
    std::array<uint8_t, 32> result;
    TaggedHash(tag.c_str(), data, len, result.data());
    return result;
}

// Convenience overload: vector in, std::array out
inline std::array<uint8_t, 32> TaggedHashArray(const std::string& tag, const std::vector<uint8_t>& data) {
    return TaggedHashArray(tag, data.data(), data.size());
}

} // namespace crypto
} // namespace dinero

#endif // DINERO_CRYPTO_TAGGED_HASH_H
