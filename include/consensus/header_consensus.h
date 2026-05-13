#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file header_consensus.h
 * @brief Consensus-critical block header validation
 *
 * ═══════════════════════════════════════════════════════════════════════════════
 *  CONSENSUS-FINAL: BlockHeader v1 = 128 bytes
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Dinero uses 128-byte block headers (BlockHeader v1) from genesis.
 * This is a CONSENSUS-FINAL decision - changing this requires a hard fork.
 *
 * Header format:
 *   - version (4) + prevHash (32) + merkleRoot (32) + utreexoRoot (32)
 *   - timestamp (8) + difficulty (4) + nonce (4) + reserved (12) = 128 bytes
 *
 * PoW domain: SHA256d covers ALL 128 bytes (not just first 80).
 *
 * This file provides explicit consensus checks to prevent malformed blocks.
 */

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════════
// CONSENSUS-FINAL: Header Size = 128 bytes (ABI v2)
// ═══════════════════════════════════════════════════════════════════════════════
constexpr size_t HEADER_SIZE = 128;  // CONSENSUS-FINAL: BlockHeader v1

// ABI version for header format (bump when header layout changes)
constexpr uint32_t HEADER_ABI_VERSION = 2;

// Legacy sizes (for reference only - NOT accepted)
constexpr size_t LEGACY_BLOCK_HEADER_SIZE = 80;    // Bitcoin-compatible (rejected)
constexpr size_t TRANSITIONAL_HEADER_SIZE = 112;   // Deprecated (rejected)
constexpr size_t CURRENT_BLOCK_HEADER_SIZE = HEADER_SIZE;  // Alias for compatibility
constexpr size_t UTREEXO_COMMITMENT_SIZE = 32;     // Size of Utreexo root field

// Version that activated BlockHeader v1 (128 bytes)
constexpr uint32_t BLOCKHEADER_V1_ACTIVATION_VERSION = 1;

// ═══════════════════════════════════════════════════════════════════════════════
// CONSENSUS-FINAL GUARD: This stops churn. Do not modify.
// ═══════════════════════════════════════════════════════════════════════════════
static_assert(HEADER_SIZE == 128, "CONSENSUS-FINAL: Header size is 128 bytes");

// Additional consensus guards
static_assert(UTREEXO_COMMITMENT_SIZE == 32, "Utreexo root must be 32 bytes");
static_assert(LEGACY_BLOCK_HEADER_SIZE != HEADER_SIZE, "Legacy headers must differ from current");

/**
 * @brief Validate block header size (consensus-critical)
 * @param header_size Size of block header in bytes
 * @param block_version Block version field
 * @param height Block height (for future activation logic)
 * @return true if header size is valid for this block
 *
 * Consensus rules:
 * - All blocks MUST use 128-byte headers (BlockHeader v1)
 * - 80-byte and 112-byte headers are REJECTED
 */
inline bool IsValidHeaderSize(size_t header_size, uint32_t block_version, uint32_t height) {
    (void)height;  // Reserved for future activation logic

    // Dinero consensus: All blocks MUST use 128-byte headers (BlockHeader v1)
    if (block_version >= BLOCKHEADER_V1_ACTIVATION_VERSION) {
        return header_size == CURRENT_BLOCK_HEADER_SIZE;
    }

    // Explicitly reject legacy 80-byte headers
    // (This prevents silent acceptance of malformed blocks)
    if (header_size == LEGACY_BLOCK_HEADER_SIZE) {
        return false;  // REJECT: 80-byte headers not supported
    }

    return header_size == CURRENT_BLOCK_HEADER_SIZE;
}

/**
 * @brief Get expected header size for block (consensus-critical)
 * @param block_version Block version field
 * @param height Block height
 * @return Expected header size in bytes
 */
inline size_t GetExpectedHeaderSize(uint32_t block_version, uint32_t height) {
    (void)block_version;
    (void)height;

    // Dinero: Always 128 bytes (BlockHeader v1 active from genesis)
    return CURRENT_BLOCK_HEADER_SIZE;
}

/**
 * @brief Validate PoW hash coverage (consensus-critical)
 * @param header_size Size of header that was hashed
 * @param block_version Block version
 * @return true if PoW correctly covers the full header
 *
 * Consensus rule:
 * - PoW MUST cover all 128 bytes (BlockHeader v1: includes Utreexo root + reserved field)
 * - Hashing only 80 or 112 bytes is INVALID (would allow field manipulation)
 */
inline bool IsValidPoWCoverage(size_t header_size, uint32_t block_version) {
    (void)block_version;

    // PoW must cover ENTIRE 128-byte header (BlockHeader v1)
    return header_size == CURRENT_BLOCK_HEADER_SIZE;
}

} // namespace consensus
} // namespace dinero
