#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
//  Dinero Midstate Caching Engine
//
//  Provides SHA256 midstate computation for 112-byte block headers.
//  Miners can cache the midstate after processing the first 64 bytes,
//  then only compute the final rounds when iterating through nonces.
//
//  Dinero 112-byte Header SHA256 Layout:
//    Block 1 (bytes 0-63):   version + prevhash + partial merkle [MIDSTATE HERE]
//    Block 2 (bytes 64-111): remaining merkle + time + bits + nonce + utreexo + padding
//
//  For each job, Block 1 is constant - only Block 2 changes (nonce iteration).
//  This provides significant speedup for GPU/ASIC miners.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>
#include "mining/header_layout.h"

namespace dinero {
namespace mining {

/**
 * SHA256 Midstate - intermediate hash state after processing N blocks
 */
struct SHA256Midstate {
    uint32_t state[8];      // SHA256 internal state (256 bits)
    uint64_t bytes_processed;  // Number of bytes hashed (must be multiple of 64)

    // Serialization for network transmission
    std::string ToHex() const;
    static SHA256Midstate FromHex(const std::string& hex);
};

/**
 * Mining Job with Midstate Caching
 *
 * Contains all data needed for a miner to work on a block:
 * - midstate: SHA256 state after first 64 bytes
 * - block2_data: remaining 48 bytes (header bytes 64-111)
 * - Precomputed padding for block 2
 */
struct MiningJobMidstate {
    // Job identification
    std::string job_id;

    // SHA256 midstate after first 64 bytes of header
    SHA256Midstate midstate;

    // Block 2 data: bytes 64-111 of header (48 bytes)
    // Layout:
    //   [0-3]   merkle_root[28:31] (last 4 bytes of merkle root)
    //   [4-7]   timestamp
    //   [8-11]  bits
    //   [12-15] nonce  <-- MINERS MODIFY THIS
    //   [16-47] utreexo_commitment
    uint8_t block2_data[DINERO_SHA256_BLOCK2_PAYLOAD];  // 48 bytes

    // Precomputed block 2 with padding (64 bytes total for SHA256)
    // Layout:
    //   [0-47]  block2_data
    //   [48]    0x80 (padding start)
    //   [49-61] 0x00 (zeros)
    //   [62-63] length in bits (896 = 0x0380, big-endian)
    uint8_t block2_padded[DINERO_SHA256_BLOCK_SIZE];  // 64 bytes

    // Index of nonce within block2_data (offset 12)
    static constexpr size_t NONCE_OFFSET = 12;

    // Difficulty target
    uint32_t nbits;
    double target_difficulty;

    // Full 112-byte header (for block submission after finding solution)
    uint8_t full_header[DINERO_HEADER_SIZE_BYTES];

    // Coinbase and merkle data for share reconstruction
    std::string extranonce1;
    std::string extranonce2_placeholder;
    std::vector<std::string> merkle_branches;
};

/**
 * Midstate Cache Engine
 *
 * Computes and caches SHA256 midstates for mining jobs.
 */
class MidstateCache {
public:
    /**
     * Compute midstate from first 64 bytes of a 128-byte header
     *
     * @param header128 Full 128-byte Dinero block header
     * @return SHA256 midstate after first 64 bytes
     */
    static SHA256Midstate ComputeMidstate(const uint8_t header128[DINERO_HEADER_SIZE_BYTES]);

    /**
     * Create a mining job with midstate from a 128-byte header
     *
     * @param header128 Full 128-byte block header
     * @param job_id Job identifier string
     * @param nbits Difficulty target bits
     * @return MiningJobMidstate ready for miners
     */
    static MiningJobMidstate CreateMiningJob(
        const uint8_t header128[DINERO_HEADER_SIZE_BYTES],
        const std::string& job_id,
        uint32_t nbits
    );

    /**
     * Complete SHA256d hash from midstate + block2 data
     *
     * @param midstate SHA256 state after first 64 bytes
     * @param block2_padded Remaining 48 bytes + padding (64 bytes total)
     * @param hash_out Output buffer for 32-byte hash
     */
    static void CompleteHash(
        const SHA256Midstate& midstate,
        const uint8_t block2_padded[DINERO_SHA256_BLOCK_SIZE],
        uint8_t hash_out[32]
    );

    /**
     * Complete SHA256d hash with specific nonce value
     *
     * @param midstate SHA256 state after first 64 bytes
     * @param block2_data Block 2 payload (48 bytes, will modify nonce in copy)
     * @param nonce Nonce value to test
     * @param hash_out Output buffer for 32-byte hash
     */
    static void CompleteHashWithNonce(
        const SHA256Midstate& midstate,
        const uint8_t block2_data[DINERO_SHA256_BLOCK2_PAYLOAD],
        uint32_t nonce,
        uint8_t hash_out[32]
    );

    /**
     * Verify a hash meets difficulty target
     *
     * @param hash 32-byte hash (little-endian)
     * @param nbits Compact difficulty target
     * @return true if hash <= target
     */
    static bool MeetsDifficulty(const uint8_t hash[32], uint32_t nbits);

    /**
     * Convert difficulty bits to target as 32-byte value
     *
     * @param nbits Compact difficulty target
     * @param target_out Output buffer for 32-byte target (big-endian)
     */
    static void BitsToTarget(uint32_t nbits, uint8_t target_out[32]);

private:
    /**
     * Prepare block 2 with SHA256 padding
     */
    static void PrepareBlock2Padded(
        const uint8_t block2_data[DINERO_SHA256_BLOCK2_PAYLOAD],
        uint8_t block2_padded[DINERO_SHA256_BLOCK_SIZE]
    );
};

} // namespace mining
} // namespace dinero
