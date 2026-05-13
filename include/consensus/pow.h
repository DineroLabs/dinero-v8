#pragma once

#include "primitives/block.h"
#include <cstdint>
#include <vector>
#include <string>

namespace dinero {
namespace consensus {

/**
 * @file pow.h
 * @brief Bitcoin Core-compatible Proof-of-Work verification
 *
 * Implements full PoW validation for Dinero headers:
 * 1. Compact bits to target conversion
 * 2. Double SHA-256 hashing
 * 3. Hash vs target comparison (little-endian)
 *
 * Compatible with Bitcoin Core's CheckProofOfWork() in pow.cpp
 */

// ═══════════════════════════════════════════════════════════════════════════
// Constants
// ═══════════════════════════════════════════════════════════════════════════

// Maximum target (difficulty = 1) - Dinero unified difficulty (50× easier than Bitcoin)
// This is the floor for all blocks - genesis, bootstrap, and ASERT phases
// 0x1d31ffce = exponent 0x1d (29 bytes), mantissa 0x31ffce
static constexpr uint32_t MAX_BITS = 0x1d31ffce;

// Minimum target (highest possible difficulty)
// All zeros except one bit set (extremely difficult)
static constexpr uint32_t MIN_BITS = 0x01010000;

// ═══════════════════════════════════════════════════════════════════════════
// Core PoW Functions
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Convert compact "bits" representation to 256-bit target
 *
 * Bitcoin's compact format encodes a 256-bit target as 32 bits:
 * - bits[31:24]: exponent (size in bytes)
 * - bits[23:0]:  mantissa (first 3 significant bytes)
 *
 * Formula: target = mantissa × 256^(exponent - 3)
 *
 * Example: bits = 0x1d31ffce (Dinero unified difficulty)
 * - exponent = 0x1d (29 bytes)
 * - mantissa = 0x31ffce
 * - target = 0x31ffce × 256^(29-3) = 0x31ffce × 256^26
 *          = 0x0000000031ffce00000000000000000000000000000000000000000000000000
 *
 * @param bits Compact difficulty bits
 * @return 256-bit target as 32-byte vector (big-endian)
 */
std::vector<uint8_t> BitsToTarget(uint32_t bits);

/**
 * @brief Compute double SHA-256 hash of block header
 *
 * Bitcoin block hash = SHA256(SHA256(header_bytes))
 *
 * Header serialization (112 bytes total, Dinero canonical):
 * - version (4 bytes, little-endian)
 * - prev_block_hash (32 bytes, little-endian bytes from hex)
 * - merkle_root (32 bytes, little-endian bytes from hex)
 * - timestamp/time (4 bytes, little-endian)
 * - bits/difficulty (4 bytes, little-endian)
 * - nonce (4 bytes, little-endian)
 * - utreexo_commitment (32 bytes, little-endian bytes from hex)
 *
 * @param header Block header to hash
 * @return 32-byte hash (internal byte order)
 */
std::vector<uint8_t> GetBlockHash(const BlockHeader& header);

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
 *
 * @param hash 32-byte block hash (big-endian)
 * @param target 32-byte target (big-endian)
 * @return true if hash <= target
 */
bool HashMeetsTarget(const std::vector<uint8_t>& hash, const std::vector<uint8_t>& target);

/**
 * @brief Bitcoin Core-compatible Proof-of-Work validation
 *
 * Complete PoW check:
 * 1. Extract difficulty bits from header
 * 2. Convert bits to 256-bit target
 * 3. Compute double SHA-256(header)
 * 4. Check: hash <= target
 *
 * @param header Block header to validate
 * @param require_standard If true, enforce standard difficulty limits (default: true)
 * @return true if proof-of-work is valid
 */
bool CheckProofOfWork(const BlockHeader& header, bool require_standard = true);

/**
 * @brief Validate difficulty bits are within allowed range
 *
 * Bitcoin consensus rules:
 * - bits must decode to a valid target
 * - target must not be negative (sign bit check)
 * - target must be <= maximum target (difficulty >= 1)
 *
 * @param bits Compact difficulty bits
 * @return true if bits are valid
 */
bool CheckDifficultyBits(uint32_t bits);

// ═══════════════════════════════════════════════════════════════════════════
// Utility Functions
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Serialize block header to canonical 128-byte format
 *
 * @param header Block header
 * @return 128-byte serialized header
 */
std::vector<uint8_t> SerializeHeader(const BlockHeader& header);

/**
 * @brief Convert hash from hex string to bytes (handles byte order)
 *
 * @param hex_hash Hex-encoded hash string
 * @return 32-byte hash vector
 */
std::vector<uint8_t> HashFromHex(const std::string& hex_hash);

/**
 * @brief Convert hash bytes to hex string
 *
 * @param hash 32-byte hash vector
 * @return Hex-encoded hash string
 */
std::string HashToHex(const std::vector<uint8_t>& hash);

} // namespace consensus
} // namespace dinero
