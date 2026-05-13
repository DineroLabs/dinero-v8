#pragma once

#include "types.h"
#include <cstdint>

namespace dinero {
namespace solo {

/**
 * SHA256d (double SHA256) hash engine
 *
 * Uses OpenSSL for SHA256 implementation.
 */
class HashEngine {
public:
    /**
     * Compute SHA256d of data
     * SHA256d = SHA256(SHA256(data))
     */
    static Hash256 sha256d(const uint8_t* data, size_t len);

    /**
     * Compute SHA256d of 128-byte block header
     * Optimized for mining loop.
     */
    static Hash256 hashHeader(const uint8_t* header);

    /**
     * Single SHA256 (for merkle tree, etc.)
     */
    static Hash256 sha256(const uint8_t* data, size_t len);

    /**
     * Compute merkle root from transaction hashes
     */
    static Hash256 computeMerkleRoot(const std::vector<Hash256>& txids);
};

} // namespace solo
} // namespace dinero
