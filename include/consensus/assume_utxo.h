#pragma once

#include "primitives/uint256.h"
#include "consensus/chainwork.h"
#include <vector>
#include <optional>
#include <filesystem>

namespace dinero {
namespace consensus {

/**
 * AssumeUTXO: Fast Sync with Background Validation
 *
 * Enables instant wallet functionality by loading a trusted UTXO snapshot,
 * then validating historical blocks in the background.
 *
 * DESIGN:
 * 1. Ship software with hardcoded snapshot hashes (this file)
 * 2. On startup with --snapshot, load snapshot and verify hash
 * 3. Wallet becomes usable immediately (assume snapshot is valid)
 * 4. Background: validate blocks from genesis to snapshot height
 * 5. When validation completes, switch to fully-validated chain
 *
 * SECURITY:
 * - Snapshot hash is hardcoded (consensus-critical)
 * - Background validation proves the chain
 * - If validation fails, node shuts down (snapshot was bad)
 * - No consensus changes (same validation rules)
 *
 * Based on Bitcoin Core's AssumeUTXO (BIP assumed-valid).
 */

/**
 * Snapshot metadata for AssumeUTXO
 *
 * This structure defines a known-good snapshot at a specific height.
 * Values are hardcoded in this file (consensus-critical).
 */
struct AssumeUTXOSnapshot {
    // Snapshot file SHA256 hash (entire file including checksum)
    // This is the trust anchor - if file matches, we assume it's valid
    uint256 snapshot_hash;

    // Block hash at this snapshot
    uint256 block_hash;

    // Block height
    uint32_t height;

    // Chainwork at this height (for fork choice)
    arith_uint256 chainwork;

    // Number of UTXOs in snapshot (sanity check)
    uint64_t utxo_count;

    // Human-readable description
    std::string description;

    AssumeUTXOSnapshot(
        const std::string& snapshot_hash_hex,
        const std::string& block_hash_hex,
        uint32_t h,
        const std::string& chainwork_hex,
        uint64_t utxos,
        const std::string& desc
    );
};

/**
 * Registry of known-good snapshots
 *
 * These are hardcoded consensus-critical values.
 * Only modify when adding new snapshots (never change existing).
 *
 * Process for adding new snapshot:
 * 1. Sync node to height H
 * 2. Export snapshot: chain_manager->ExportSnapshot("snapshot_H.dat")
 * 3. Compute file hash: sha256sum snapshot_H.dat
 * 4. Get block hash at height H
 * 5. Get chainwork at height H
 * 6. Add entry here
 * 7. Distribute snapshot file (out-of-band)
 */
class AssumeUTXORegistry {
public:
    /**
     * Get snapshot for a specific height
     *
     * @param height Block height
     * @return Snapshot metadata if exists, nullopt otherwise
     */
    static std::optional<AssumeUTXOSnapshot> GetSnapshot(uint32_t height);

    /**
     * Get all available snapshots
     *
     * @return Vector of all registered snapshots
     */
    static std::vector<AssumeUTXOSnapshot> GetAllSnapshots();

    /**
     * Verify a snapshot file matches expected hash
     *
     * Computes SHA256 of entire file and compares to registered hash.
     *
     * @param snapshot_path Path to snapshot file
     * @param expected_height Expected height of snapshot
     * @return true if hash matches, false otherwise
     */
    static bool VerifySnapshotHash(const std::filesystem::path& snapshot_path, uint32_t expected_height);

private:
    // Registry of known-good snapshots
    // These are hardcoded consensus-critical values
    static const std::vector<AssumeUTXOSnapshot> snapshots_;
};

//=============================================================================
// HARDCODED SNAPSHOT REGISTRY (Consensus-Critical)
//=============================================================================
//
// ⚠️ WARNING: These values are consensus-critical trust anchors ⚠️
//
// Process for adding snapshots:
// 1. Fully sync node to desired height
// 2. Export snapshot using ExportSnapshot()
// 3. Compute file hash: sha256sum snapshot_HEIGHT.dat
// 4. Get block hash: getblockhash HEIGHT
// 5. Get chainwork from block index
// 6. Add entry below
// 7. Test snapshot import + verification
// 8. Distribute snapshot file
//
// NEVER modify existing entries (breaks trust).
// Only ADD new snapshots.
//
//=============================================================================

// Mainnet height 8138 (Fair Launch v3, Mar 2026):
// - Snapshot SHA256: d8c22bf3d832a9b02fbfc2d78940e457694066ef4b5bd9479042c376544eabaf
// - Block hash: 0000000001c138b0af640c74ff85f300fad376d73a131ea95287dc29326b7343
// - Chainwork: 0x2f53e4e86a38
// - UTXO count: 7,862
// - File size: 945,967 bytes
// - Generated: Mar 12, 2026 on DineroLA via dumptxoutset RPC
//
// Mainnet height 27727 (Apr 9 2026):
// - Snapshot SHA256: 21a27b9a12a6d767039246b9b39c90301644d1dca73f94f4495945f5a01b3858
// - Block hash: 00000000790bbd5855d9062cfb783a418f8f639b5052c9a542c9358b979f43e5
// - Chainwork: 89d300000000167743af73e4bf4744204b29f5347aaaf0a34d46d436c0e80a3f
// - UTXO count: 60,913
// - File size: 7,496,212 bytes
// - Generated: Apr 9, 2026 on DineroVA via dumptxoutset RPC

} // namespace consensus
} // namespace dinero
