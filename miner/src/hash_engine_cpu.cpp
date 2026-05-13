#include "solo_miner/hash_engine.h"
#include <openssl/evp.h>
#include <cstring>

namespace dinero {
namespace solo {

Hash256 HashEngine::sha256(const uint8_t* data, size_t len) {
    Hash256 hash;
    unsigned int hash_len = 32;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
    EVP_MD_CTX_free(ctx);

    return hash;
}

Hash256 HashEngine::sha256d(const uint8_t* data, size_t len) {
    // First SHA256
    Hash256 intermediate;
    unsigned int hash_len = 32;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, intermediate.data(), &hash_len);

    // Second SHA256
    Hash256 result;
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, intermediate.data(), 32);
    EVP_DigestFinal_ex(ctx, result.data(), &hash_len);
    EVP_MD_CTX_free(ctx);

    return result;
}

Hash256 HashEngine::hashHeader(const uint8_t* header) {
    // Hash 128-byte block header with SHA256d
    return sha256d(header, HEADER_SIZE);
}

Hash256 HashEngine::computeMerkleRoot(const std::vector<Hash256>& txids) {
    if (txids.empty()) {
        return Hash256{};
    }

    if (txids.size() == 1) {
        return txids[0];
    }

    std::vector<Hash256> current = txids;

    while (current.size() > 1) {
        std::vector<Hash256> next;
        next.reserve((current.size() + 1) / 2);

        for (size_t i = 0; i < current.size(); i += 2) {
            // If odd number of hashes, duplicate the last one
            const Hash256& left = current[i];
            const Hash256& right = (i + 1 < current.size()) ? current[i + 1] : current[i];

            // Concatenate and hash
            uint8_t combined[64];
            std::memcpy(combined, left.data(), 32);
            std::memcpy(combined + 32, right.data(), 32);

            next.push_back(sha256d(combined, 64));
        }

        current = std::move(next);
    }

    return current[0];
}

} // namespace solo
} // namespace dinero
