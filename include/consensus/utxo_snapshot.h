#pragma once

#include "primitives/uint256.h"
#include <string>
#include <cstdint>
#include <filesystem>

namespace dinero {
namespace consensus {

/**
 * UTXO Snapshot Format
 *
 * Enables fast sync by loading a verified UTXO set state at a specific block.
 *
 * Format:
 * ┌─────────────────────────────────────┐
 * │ HEADER (68 bytes)                   │
 * ├─────────────────────────────────────┤
 * │ Magic: "UTXO" (4 bytes)             │
 * │ Version: 2/3 (4 bytes)              │
 * │ Block Hash (32 bytes)               │
 * │ Block Height (4 bytes)              │
 * │ UTXO Count (8 bytes)                │
 * │ Timestamp (8 bytes)                 │
 * │ Reserved (8 bytes)                  │
 * ├─────────────────────────────────────┤
 * │ UTXO ENTRIES (variable)             │
 * ├─────────────────────────────────────┤
 * │ For each UTXO:                      │
 * │   - txid (32 bytes)                 │
 * │   - vout (4 bytes)                  │
 * │   - value (8 bytes)                 │
 * │   - scriptPubKey length (4 bytes)   │
 * │   - scriptPubKey (variable)         │
 * │   - height (4 bytes)                │
 * │   - isCoinbase (1 byte)             │
 * ├─────────────────────────────────────┤
 * │ OPTIONAL V3 Utreexo Section         │
 * ├─────────────────────────────────────┤
 * │ Section Magic: "UTRX" (4 bytes)     │
 * │ Section Version: 1 (4 bytes)        │
 * │ Forest byte size (8 bytes)          │
 * │ Forest leaves (8 bytes)             │
 * │ Reserved (8 bytes)                  │
 * │ Utreexo root (32 bytes)             │
 * │ Serialized forest (variable)        │
 * ├─────────────────────────────────────┤
 * │ FOOTER                              │
 * ├─────────────────────────────────────┤
 * │ Checksum: SHA256 (32 bytes)         │
 * └─────────────────────────────────────┘
 *
 * Checksum is SHA256 of (HEADER + all UTXO entries).
 *
 * Design Principles:
 * - Simple, deterministic serialization
 * - No compression (can be added later)
 * - Verifiable (checksum)
 * - Version field for future upgrades
 * - Reserved field for metadata expansion
 */

// Snapshot file magic number
constexpr uint32_t SNAPSHOT_MAGIC = 0x4F545855;  // "UTXO" in little-endian

// Snapshot format version (v2: 128-byte headers)
constexpr uint32_t SNAPSHOT_VERSION_V2 = 2;
constexpr uint32_t SNAPSHOT_VERSION_V3 = 3;
constexpr uint32_t SNAPSHOT_VERSION = SNAPSHOT_VERSION_V3;

// Optional v3 section carrying the accumulator bootstrap payload.
constexpr uint32_t SNAPSHOT_V3_UTREEXO_MAGIC = 0x58525455;  // "UTRX" in little-endian
constexpr uint32_t SNAPSHOT_V3_UTREEXO_SECTION_VERSION = 1;
constexpr uint64_t SNAPSHOT_V3_MAX_FOREST_BYTES = (1ULL << 30);  // 1 GiB hard cap

// Snapshot header size (fixed)
constexpr size_t SNAPSHOT_HEADER_SIZE = 68;

// Snapshot footer size (fixed)
constexpr size_t SNAPSHOT_FOOTER_SIZE = 32;

/**
 * Snapshot metadata (header)
 */
struct SnapshotMetadata {
    uint32_t magic;           // Magic number (0x4F545855 = "UTXO")
    uint32_t version;         // Format version (2 or 3)
    uint256 block_hash;       // Block hash this snapshot corresponds to
    uint32_t block_height;    // Block height
    uint64_t utxo_count;      // Number of UTXOs in snapshot
    uint64_t timestamp;       // Unix timestamp when snapshot was created
    uint64_t reserved;        // Reserved for future use

    SnapshotMetadata()
        : magic(SNAPSHOT_MAGIC)
        , version(SNAPSHOT_VERSION)
        , block_hash()
        , block_height(0)
        , utxo_count(0)
        , timestamp(0)
        , reserved(0)
    {}
};

/**
 * Optional v3 section metadata for direct Utreexo bootstrap.
 */
struct SnapshotUtreexoSection {
    uint32_t magic;          // SNAPSHOT_V3_UTREEXO_MAGIC
    uint32_t version;        // SNAPSHOT_V3_UTREEXO_SECTION_VERSION
    uint64_t forest_bytes;   // Serialized forest payload size
    uint64_t forest_leaves;  // Expected leaf count (sanity check)
    uint64_t reserved;       // Reserved for future fields
    uint256 utreexo_root;    // Forest commitment at snapshot base block

    SnapshotUtreexoSection()
        : magic(SNAPSHOT_V3_UTREEXO_MAGIC)
        , version(SNAPSHOT_V3_UTREEXO_SECTION_VERSION)
        , forest_bytes(0)
        , forest_leaves(0)
        , reserved(0)
        , utreexo_root()
    {}
};

/**
 * Snapshot export result
 */
struct SnapshotExportResult {
    bool success;
    std::string error_message;
    uint64_t utxos_exported;
    uint64_t bytes_written;
    uint256 checksum;
    uint256 block_hash;      // Block hash at which snapshot was taken
    uint32_t block_height;   // Block height at which snapshot was taken
};

/**
 * Snapshot import result
 */
struct SnapshotImportResult {
    bool success;
    std::string error_message;
    uint64_t utxos_imported;
    uint64_t bytes_read;
    uint256 block_hash;
    uint32_t block_height;
    bool checksum_valid;
};

} // namespace consensus
} // namespace dinero
