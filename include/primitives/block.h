#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "mining/header_layout.h"  // Canonical 128-byte header layout (Phase 2: BlockHeader v1)
#include "primitives/transaction.h"    // Include Transaction definition
#include "primitives/uint256.h"    // Phase M.0: uint256 is identity
#include "consensus/utreexo_accumulator.h"  // BlockUtreexoData

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════════
// CONSENSUS-CRITICAL INVARIANT: BlockHeader v1 (128 bytes) - FROZEN
// ═══════════════════════════════════════════════════════════════════════════════
// Dinero BlockHeader v1 is a clean, 128-byte consensus format with no legacy
// duplication. This format is IMMUTABLE and requires a hard fork to change.
//
// Layout (128 bytes):
//   Offset 0x00 (4 bytes):   version
//   Offset 0x04 (32 bytes):  prev_block_hash
//   Offset 0x24 (32 bytes):  merkle_root
//   Offset 0x44 (32 bytes):  utreexo_root
//   Offset 0x64 (8 bytes):   timestamp
//   Offset 0x6C (4 bytes):   difficulty
//   Offset 0x70 (4 bytes):   nonce
//   Offset 0x74 (12 bytes):  reserved (MUST be zero)
//
// Consensus Rules:
//   - Header MUST be exactly 128 bytes
//   - Header hash = double SHA-256 of all 128 bytes (no truncation)
//   - reserved[12] MUST be all zero bytes (non-zero → invalid block)
//   - Any layout change requires hard fork
//
// Properties:
//   - Trivially copyable (memcpy-safe)
//   - Cache-line aligned (128 = 2^7 bytes)
//   - Deterministic serialization (little-endian for all multi-byte fields)
//
// See: docs/BLOCKHEADER_V1_FINALIZATION_PLAN.md
// ═══════════════════════════════════════════════════════════════════════════════
#pragma pack(push, 1)
struct BlockHeader {
    uint32_t version;              // Block version (4 bytes, offset 0x00)
    uint256 prev_block_hash;       // Previous block hash (32 bytes, offset 0x04)
    uint256 merkle_root;           // Merkle root of transactions (32 bytes, offset 0x24)
    uint256 utreexo_root;          // Utreexo accumulator root (32 bytes, offset 0x44)
    uint64_t timestamp;            // Unix timestamp (8 bytes, offset 0x64)
    uint32_t difficulty;           // Compact difficulty target (4 bytes, offset 0x6C)
    uint32_t nonce;                // Mining nonce (4 bytes, offset 0x70)
    uint8_t reserved[12];          // Reserved for future use (12 bytes, offset 0x74, MUST be zero)

    std::array<uint8_t, 128> SerializeForHash() const;
    std::string Serialize() const;

    /**
     * @brief Deserialize BlockHeader from raw bytes
     *
     * Parses 128 bytes in little-endian format to reconstruct BlockHeader.
     * Returns std::nullopt if data is malformed or too short.
     *
     * @param data Raw bytes (must be at least 128 bytes)
     * @return std::optional<BlockHeader> Deserialized header or nullopt
     */
    static std::optional<BlockHeader> Deserialize(const std::vector<uint8_t>& data);
    static std::optional<BlockHeader> Deserialize(const uint8_t* data, size_t len);

    /**
     * @brief Get block hash (Phase M.0 compliant - returns uint256)
     *
     * Phase M.0: uint256 is identity, .GetHex() is presentation.
     * This returns the binary hash. Use .GetHex() for logging/RPC.
     *
     * @return uint256 Block hash (binary)
     */
    uint256 GetHash() const;

    /**
     * @brief Check if reserved field is all zeros (BlockHeader v1 consensus rule)
     *
     * BlockHeader v1 requires reserved[12] to be all zeros.
     * Any non-zero byte makes the block invalid.
     *
     * @return true if all reserved bytes are zero, false otherwise
     */
    bool IsReservedValid() const {
        for (int i = 0; i < 12; i++) {
            if (reserved[i] != 0) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Initialize reserved field to zeros (defensive programming)
     *
     * Call this after constructing BlockHeader to ensure reserved field is zeroed.
     * Note: C++ zero-initialization with {} already zeros reserved, but this is explicit.
     */
    void ZeroReserved() {
        std::memset(reserved, 0, sizeof(reserved));
    }
};
#pragma pack(pop)

// BlockHeader v1: Compile-time guarantees
static_assert(sizeof(BlockHeader) == 128,
              "BlockHeader v1 MUST be exactly 128 bytes");
static_assert(std::is_trivially_copyable_v<BlockHeader>,
              "BlockHeader v1 MUST be trivially copyable (memcpy-safe)");
static_assert(offsetof(BlockHeader, version) == 0x00, "version offset must be 0x00");
static_assert(offsetof(BlockHeader, prev_block_hash) == 0x04, "prev_block_hash offset must be 0x04");
static_assert(offsetof(BlockHeader, merkle_root) == 0x24, "merkle_root offset must be 0x24");
static_assert(offsetof(BlockHeader, utreexo_root) == 0x44, "utreexo_root offset must be 0x44");
static_assert(offsetof(BlockHeader, timestamp) == 0x64, "timestamp offset must be 0x64");
static_assert(offsetof(BlockHeader, difficulty) == 0x6C, "difficulty offset must be 0x6C");
static_assert(offsetof(BlockHeader, nonce) == 0x70, "nonce offset must be 0x70");
static_assert(offsetof(BlockHeader, reserved) == 0x74, "reserved offset must be 0x74");

struct Block {
    BlockHeader header;
    std::vector<Transaction> vtx; // actual transaction objects

    /**
     * @brief Optional Utreexo proof data (Phase 1: Proof Data Structures)
     *
     * Contains:
     * - Batched proof for all spent inputs in this block
     * - Accumulator root BEFORE applying this block
     *
     * Phase 1: Blocks can carry this data (may be empty)
     * Phase 2: Shadow verification (verify but don't enforce)
     * Phase 3: Enforce at activation height
     *
     * nullopt = no Utreexo data (backward compatibility)
     * empty BlockUtreexoData = Utreexo active but no spends (coinbase-only block)
     */
    std::optional<consensus::BlockUtreexoData> utreexo;

    std::string Serialize() const;

    /**
     * @brief Deserialize Block from Dinero wire format (Block::Serialize output)
     *
     * Format:
     *   - 128-byte BlockHeader v1
     *   - CompactSize tx count
     *   - Transactions (with witness, if present)
     *   - Optional Utreexo flag + payload (0x00 or 0x01 + bytes)
     *
     * Backward compatibility:
     *   - If the Utreexo flag is missing (older blocks), treats as "no Utreexo data".
     *
     * @param data Raw block bytes (must be at least 128 bytes)
     * @return std::optional<Block> Parsed block or nullopt on parse failure
     */
    static std::optional<Block> Deserialize(const std::vector<uint8_t>& data);
    static std::optional<Block> Deserialize(const uint8_t* data, size_t len);

    /**
     * @brief Get block hash (Phase M.0 compliant - returns uint256)
     *
     * Phase M.0: uint256 is identity, .GetHex() is presentation.
     * This returns the binary hash. Use .GetHex() for logging/RPC.
     *
     * @return uint256 Block hash (binary)
     */
    uint256 GetHash() const;
};

} // namespace dinero
