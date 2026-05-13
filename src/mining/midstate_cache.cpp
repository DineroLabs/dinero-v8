#include "mining/midstate_cache.h"
#include "crypto/sha256.h"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace mining {

// ─────────────────────────────────────────────────────────────────────────────
// SHA256Midstate serialization
// ─────────────────────────────────────────────────────────────────────────────

std::string SHA256Midstate::ToHex() const {
    std::stringstream ss;
    // Serialize state as 8 big-endian 32-bit words
    for (int i = 0; i < 8; i++) {
        ss << std::hex << std::setfill('0') << std::setw(8) << state[i];
    }
    return ss.str();
}

SHA256Midstate SHA256Midstate::FromHex(const std::string& hex) {
    SHA256Midstate result;
    result.bytes_processed = 64;  // Standard midstate after 64 bytes

    if (hex.length() != 64) {
        // Invalid hex, return zeroed midstate
        std::memset(result.state, 0, sizeof(result.state));
        return result;
    }

    for (int i = 0; i < 8; i++) {
        std::string word = hex.substr(i * 8, 8);
        result.state[i] = static_cast<uint32_t>(std::stoul(word, nullptr, 16));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// MidstateCache implementation
// ─────────────────────────────────────────────────────────────────────────────

SHA256Midstate MidstateCache::ComputeMidstate(const uint8_t header112[DINERO_HEADER_SIZE_BYTES]) {
    SHA256Midstate result;

    // Initialize SHA256 with standard IV
    crypto::CSHA256 sha;

    // Process first 64 bytes (Block 1)
    sha.TransformBlock(header112);

    // Extract midstate
    sha.GetMidstate(result.state);
    result.bytes_processed = DINERO_SHA256_BLOCK_SIZE;  // 64 bytes

    return result;
}

MiningJobMidstate MidstateCache::CreateMiningJob(
    const uint8_t header112[DINERO_HEADER_SIZE_BYTES],
    const std::string& job_id,
    uint32_t nbits
) {
    MiningJobMidstate job;

    job.job_id = job_id;
    job.nbits = nbits;

    // Copy full header for block submission
    std::memcpy(job.full_header, header112, DINERO_HEADER_SIZE_BYTES);

    // Compute midstate from first 64 bytes
    job.midstate = ComputeMidstate(header112);

    // Copy block 2 data (bytes 64-111)
    std::memcpy(job.block2_data, header112 + DINERO_SHA256_BLOCK_SIZE, DINERO_SHA256_BLOCK2_PAYLOAD);

    // Prepare padded block 2 for SHA256
    PrepareBlock2Padded(job.block2_data, job.block2_padded);

    // Convert nbits to target difficulty
    uint8_t target[32];
    BitsToTarget(nbits, target);

    // Calculate difficulty as ratio (simplified)
    // difficulty = max_target / current_target
    // For now, just store nbits - actual difficulty calculation depends on network params
    job.target_difficulty = 1.0;  // TODO: Calculate from nbits

    return job;
}

void MidstateCache::PrepareBlock2Padded(
    const uint8_t block2_data[DINERO_SHA256_BLOCK2_PAYLOAD],
    uint8_t block2_padded[DINERO_SHA256_BLOCK_SIZE]
) {
    // Clear the padded block
    std::memset(block2_padded, 0, DINERO_SHA256_BLOCK_SIZE);

    // Copy 48 bytes of header data
    std::memcpy(block2_padded, block2_data, DINERO_SHA256_BLOCK2_PAYLOAD);

    // Add padding byte (0x80) at offset 48
    block2_padded[DINERO_SHA256_BLOCK2_PAYLOAD] = 0x80;

    // Bytes 49-61 are zeros (already set by memset)

    // Add length in bits (1024 = 128 * 8) at bytes 62-63 (big-endian)
    // 1024 = 0x0400
    block2_padded[62] = 0x04;
    block2_padded[63] = 0x00;
}

void MidstateCache::CompleteHash(
    const SHA256Midstate& midstate,
    const uint8_t block2_padded[DINERO_SHA256_BLOCK_SIZE],
    uint8_t hash_out[32]
) {
    // First SHA256: start from midstate, process block 2
    crypto::CSHA256 sha1;
    sha1.SetMidstate(midstate.state, midstate.bytes_processed);
    sha1.TransformBlock(block2_padded);

    // Get intermediate hash (after first SHA256)
    uint32_t intermediate_state[8];
    sha1.GetMidstate(intermediate_state);

    // Convert state to bytes (big-endian)
    uint8_t first_hash[32];
    for (int i = 0; i < 8; i++) {
        first_hash[i * 4 + 0] = (intermediate_state[i] >> 24) & 0xFF;
        first_hash[i * 4 + 1] = (intermediate_state[i] >> 16) & 0xFF;
        first_hash[i * 4 + 2] = (intermediate_state[i] >> 8) & 0xFF;
        first_hash[i * 4 + 3] = intermediate_state[i] & 0xFF;
    }

    // Second SHA256: hash the first hash
    crypto::CSHA256 sha2;
    sha2.Write(first_hash, 32);
    sha2.Finalize(hash_out);
}

void MidstateCache::CompleteHashWithNonce(
    const SHA256Midstate& midstate,
    const uint8_t block2_data[DINERO_SHA256_BLOCK2_PAYLOAD],
    uint32_t nonce,
    uint8_t hash_out[32]
) {
    // Make a copy of block2 data and update nonce
    uint8_t block2_copy[DINERO_SHA256_BLOCK2_PAYLOAD];
    std::memcpy(block2_copy, block2_data, DINERO_SHA256_BLOCK2_PAYLOAD);

    // Update nonce at offset 12 (little-endian)
    block2_copy[MiningJobMidstate::NONCE_OFFSET + 0] = nonce & 0xFF;
    block2_copy[MiningJobMidstate::NONCE_OFFSET + 1] = (nonce >> 8) & 0xFF;
    block2_copy[MiningJobMidstate::NONCE_OFFSET + 2] = (nonce >> 16) & 0xFF;
    block2_copy[MiningJobMidstate::NONCE_OFFSET + 3] = (nonce >> 24) & 0xFF;

    // Prepare padded block
    uint8_t block2_padded[DINERO_SHA256_BLOCK_SIZE];
    PrepareBlock2Padded(block2_copy, block2_padded);

    // Complete the hash
    CompleteHash(midstate, block2_padded, hash_out);
}

void MidstateCache::BitsToTarget(uint32_t nbits, uint8_t target_out[32]) {
    std::memset(target_out, 0, 32);

    // Extract exponent and mantissa from compact format
    uint32_t exp = nbits >> 24;
    uint32_t mant = nbits & 0x00ffffff;

    // Handle negative flag (high bit of mantissa)
    if (nbits & 0x00800000) {
        // Negative targets are invalid for PoW
        return;
    }

    if (exp <= 3) {
        // Target fits in 4 bytes at the end
        uint32_t v = mant >> (8 * (3 - exp));
        target_out[31] = v & 0xFF;
        target_out[30] = (v >> 8) & 0xFF;
        target_out[29] = (v >> 16) & 0xFF;
        target_out[28] = (v >> 24) & 0xFF;
    } else {
        // Target spans multiple bytes (big-endian)
        int offset = exp - 3;
        if (offset < 32) {
            target_out[32 - offset - 1] = mant & 0xFF;
            if (offset + 1 < 32) {
                target_out[32 - offset - 2] = (mant >> 8) & 0xFF;
            }
            if (offset + 2 < 32) {
                target_out[32 - offset - 3] = (mant >> 16) & 0xFF;
            }
        }
    }
}

bool MidstateCache::MeetsDifficulty(const uint8_t hash[32], uint32_t nbits) {
    uint8_t target[32];
    BitsToTarget(nbits, target);

    // Compare hash (little-endian) with target (big-endian)
    // Need to reverse hash for comparison
    for (int i = 0; i < 32; i++) {
        uint8_t hash_byte = hash[31 - i];  // Reverse hash to big-endian
        if (hash_byte < target[i]) {
            return true;  // Hash is less than target
        }
        if (hash_byte > target[i]) {
            return false;  // Hash is greater than target
        }
    }

    // Hash equals target (valid)
    return true;
}

} // namespace mining
} // namespace dinero
