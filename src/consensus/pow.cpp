#include "consensus/pow.h"
#include "crypto/sha256.h"
#include "crypto/hash.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>

/**
 * @file pow.cpp
 * @brief Bitcoin Core-compatible Proof-of-Work implementation
 *
 * This implementation follows Bitcoin Core's consensus rules exactly:
 * - Compact bits to target conversion (same as Bitcoin)
 * - Double SHA-256 block hashing (same as Bitcoin)
 * - Big-endian target comparison (same as Bitcoin)
 * - Difficulty validation (same as Bitcoin)
 */

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════
// Utility Functions
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Convert hex string to bytes
 */
std::vector<uint8_t> HashFromHex(const std::string& hex_hash) {
    std::vector<uint8_t> bytes;

    // Remove any whitespace or "0x" prefix
    std::string clean_hex = hex_hash;
    if (clean_hex.size() >= 2 && clean_hex[0] == '0' && clean_hex[1] == 'x') {
        clean_hex = clean_hex.substr(2);
    }

    // Convert pairs of hex digits to bytes
    for (size_t i = 0; i < clean_hex.length(); i += 2) {
        std::string byte_str = clean_hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        bytes.push_back(byte);
    }

    return bytes;
}

/**
 * @brief Convert bytes to hex string
 */
std::string HashToHex(const std::vector<uint8_t>& hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

/**
 * @brief Serialize block header to canonical 128-byte format (BlockHeader v1)
 */
std::vector<uint8_t> SerializeHeader(const BlockHeader& header) {
    auto bytes = header.SerializeForHash();  // Returns 128 bytes
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// ═══════════════════════════════════════════════════════════════════════════
// Compact Bits to Target Conversion
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Convert compact "bits" representation to 256-bit target
 *
 * Bitcoin's compact format:
 * - bits[31:24]: exponent (size in bytes)
 * - bits[23:0]:  mantissa (first 3 significant bytes)
 *
 * Formula: target = mantissa × 256^(exponent - 3)
 *
 * Example: bits = 0x1d00ffff
 * - exponent = 0x1d (29 bytes)
 * - mantissa = 0x00ffff
 * - target = 0x00ffff × 256^26
 */
std::vector<uint8_t> BitsToTarget(uint32_t bits) {
    // Extract exponent and mantissa
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00FFFFFF;

    // Initialize 32-byte target (256 bits) to zero
    std::vector<uint8_t> target(32, 0);

    // Validate exponent (must be 1-32 bytes)
    if (exponent < 1 || exponent > 32) {
        return target;  // Return all zeros for invalid exponent
    }

    // Check for negative target (sign bit set in mantissa)
    if (mantissa & 0x00800000) {
        return target;  // Return all zeros for negative target
    }

    // Convert mantissa to bytes (3 bytes, big-endian)
    uint8_t mantissa_bytes[3];
    mantissa_bytes[0] = (mantissa >> 16) & 0xFF;
    mantissa_bytes[1] = (mantissa >> 8) & 0xFF;
    mantissa_bytes[2] = mantissa & 0xFF;

    // Calculate position: target[32 - exponent + i]
    // This implements: target = mantissa × 256^(exponent - 3)
    if (exponent >= 3) {
        size_t offset = 32 - exponent;
        for (int i = 0; i < 3; i++) {
            if (offset + i < 32) {
                target[offset + i] = mantissa_bytes[i];
            }
        }
    } else {
        // exponent < 3: shift right (divide by 256)
        size_t offset = 32 - exponent;
        int shift = (3 - exponent) * 8;
        for (int i = 0; i < 3; i++) {
            if (offset + i < 32 && i >= (3 - exponent)) {
                target[offset + i] = mantissa_bytes[i];
            }
        }
    }

    return target;
}

// ═══════════════════════════════════════════════════════════════════════════
// Block Hash Computation
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Compute double SHA-256 hash of block header
 *
 * Bitcoin block hash = SHA256(SHA256(header_bytes))
 */
std::vector<uint8_t> GetBlockHash(const BlockHeader& header) {
    // Serialize header to canonical 128-byte format (BlockHeader v1)
    std::vector<uint8_t> serialized = SerializeHeader(header);

    // Compute double SHA-256
    std::vector<uint8_t> hash(32);

    // First SHA-256
    crypto::CSHA256 sha256_first;
    sha256_first.Write(serialized.data(), serialized.size());
    std::vector<uint8_t> first_hash = sha256_first.Finalize();

    // Second SHA-256
    crypto::CSHA256 sha256_second;
    sha256_second.Write(first_hash.data(), first_hash.size());
    hash = sha256_second.Finalize();

    return hash;
}

// ═══════════════════════════════════════════════════════════════════════════
// Target Comparison
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Check if hash meets target requirement
 *
 * Bitcoin rule: hash <= target (unsigned 256-bit integer comparison)
 *
 * Both hash and target are in big-endian for comparison:
 * - Start from most significant byte (index 0)
 * - Compare byte-by-byte
 * - If hash[i] < target[i]: valid (hash is smaller)
 * - If hash[i] > target[i]: invalid (hash is larger)
 * - If hash[i] == target[i]: continue to next byte
 */
bool HashMeetsTarget(const std::vector<uint8_t>& hash, const std::vector<uint8_t>& target) {
    // Both must be 32 bytes
    if (hash.size() != 32 || target.size() != 32) {
        return false;
    }

    // Compare byte-by-byte from most significant to least significant
    for (size_t i = 0; i < 32; i++) {
        if (hash[i] < target[i]) {
            return true;   // hash < target (valid)
        }
        if (hash[i] > target[i]) {
            return false;  // hash > target (invalid)
        }
        // If equal, continue to next byte
    }

    // All bytes equal: hash == target (valid, edge case)
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Difficulty Bits Validation
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Validate difficulty bits are within allowed range
 *
 * Bitcoin consensus rules:
 * - bits must decode to a valid target
 * - target must not be negative (sign bit check)
 * - target must be <= maximum target (difficulty >= 1)
 */
bool CheckDifficultyBits(uint32_t bits) {
    // Extract exponent and mantissa
    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x00FFFFFF;

    // Exponent must be 1-32 bytes
    if (exponent < 1 || exponent > 32) {
        return false;
    }

    // Check for negative target (sign bit set)
    if (mantissa & 0x00800000) {
        return false;
    }

    // Check if target is zero
    if (mantissa == 0 && exponent <= 3) {
        return false;
    }

    // Convert to target and check against maximum
    std::vector<uint8_t> target = BitsToTarget(bits);
    std::vector<uint8_t> max_target = BitsToTarget(MAX_BITS);

    // Target must be <= max_target (difficulty >= 1)
    // This means: max_target >= target
    // Use same comparison logic as HashMeetsTarget
    for (size_t i = 0; i < 32; i++) {
        if (target[i] < max_target[i]) {
            return true;   // target < max_target (valid)
        }
        if (target[i] > max_target[i]) {
            return false;  // target > max_target (invalid, difficulty < 1)
        }
    }

    // target == max_target (valid, difficulty = 1)
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Proof-of-Work Validation
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Bitcoin Core-compatible Proof-of-Work validation
 *
 * Complete PoW check:
 * 1. Extract difficulty bits from header
 * 2. Validate bits are within allowed range
 * 3. Convert bits to 256-bit target
 * 4. Compute double SHA-256(header)
 * 5. Check: hash <= target
 *
 * @param header Block header to validate
 * @param require_standard If true, enforce standard difficulty limits (default: true)
 * @return true if proof-of-work is valid
 */
bool CheckProofOfWork(const BlockHeader& header, bool require_standard) {
    // Extract bits (prefer new field over legacy field)
    uint32_t bits = (header.difficulty != 0) ? header.difficulty : header.difficulty;

    // Step 1: Validate difficulty bits
    if (require_standard && !CheckDifficultyBits(bits)) {
        return false;
    }

    // Step 2: Convert bits to target
    std::vector<uint8_t> target = BitsToTarget(bits);

    // Step 3: Compute block hash (double SHA-256)
    std::vector<uint8_t> hash = GetBlockHash(header);

    // Step 4: Check hash <= target
    bool valid = HashMeetsTarget(hash, target);

    return valid;
}

} // namespace consensus
} // namespace dinero
