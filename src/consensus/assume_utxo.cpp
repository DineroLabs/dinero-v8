#include "consensus/assume_utxo.h"
#include "crypto/sha256.h"
#include "common/logger.h"
#include "consensus/chainwork.h"
#include <fstream>
#include <filesystem>

namespace dinero {
namespace consensus {

//=============================================================================
// AssumeUTXOSnapshot Implementation
//=============================================================================

AssumeUTXOSnapshot::AssumeUTXOSnapshot(
    const std::string& snapshot_hash_hex,
    const std::string& block_hash_hex,
    uint32_t h,
    const std::string& chainwork_hex,
    uint64_t utxos,
    const std::string& desc
)
    : snapshot_hash(uint256::FromHexUnsafe(snapshot_hash_hex))  // Phase M.0
    , block_hash(uint256::FromHexUnsafe(block_hash_hex))  // Phase M.0
    , height(h)
    , chainwork(ChainworkFromHex(chainwork_hex))  // Use ChainworkFromHex helper
    , utxo_count(utxos)
    , description(desc)
{
}

//=============================================================================
// AssumeUTXORegistry Implementation
//=============================================================================

const std::vector<AssumeUTXOSnapshot> AssumeUTXORegistry::snapshots_ = {
    // ⚠️ NO v3/no-shielded anchors. The earlier 13000/33048/47176/52066 v3 anchors
    // were REMOVED: their snapshots carry no shielded state, so any node fast-syncing
    // from one (all are above the shielded activation height 8650) starts with an
    // empty shielded commitment tree and wedges on the first post-snapshot shielded
    // spend. Every AssumeUTXO trust anchor after activation MUST be v4+ and include
    // the SHLD section. Do NOT re-add a v3 anchor as a fast-sync path.
    //
    // Mainnet height 52241 v4 trust anchor (generated 2026-06-27 from the fleet/
    // Dell consensus tip; buries past ship time). Bundled in the desktop wallet
    // installers so fresh GUI users fast-sync instead of syncing from genesis.
    // Format: v4 UXTO + UTRX (Utreexo) + SHLD (shielded pool) snapshot — carries
    // the shielded commitment-tree frontier + anchor history + nullifier set so a
    // snapshot-bootstrapped node restores correct shielded state (tree_size=10,
    // nullifier_count=4 at dump time, root 6c45517648d707f4…) instead of an empty
    // tree that wedges on the first post-snapshot shielded spend.
    // Snapshot file: utxo-snapshot-52241.dat (19,068,210 bytes)
    //
    // ⚠️ 52241 SUPERSEDES the earlier 52066 v3 AssumeUTXO anchor (removed) because
    // v3 snapshots did NOT carry shielded state. Production AssumeUTXO anchors
    // after the shielded activation height (8650) MUST be v4+ and include the SHLD
    // section. Do NOT re-add a v3/no-shielded anchor as a production fast-sync path
    // (e.g. "cleaning up" by restoring 52066 because it looks older/smaller) — that
    // re-opens the empty-shielded-state wedge for every fresh snapshot-synced node.
    AssumeUTXOSnapshot(
        "23d987253c3eefb9d8521d6c4086350e0ac5d96e80296be4b31dbd15382063f6",
        "00000088a18ee05d5fdeaa452a1efaa1845b2d6feb8a3046c139262b7f4c2a7a",
        52241,
        "0x000000000000000000000000000000000000000000000000000005bd7927cfe5",
        154337,
        "Mainnet height 52241 v4 trust anchor (UXTO+UTRX+SHLD)"
    ),
};

std::optional<AssumeUTXOSnapshot> AssumeUTXORegistry::GetSnapshot(uint32_t height) {
    for (const auto& snapshot : snapshots_) {
        if (snapshot.height == height) {
            return snapshot;
        }
    }
    return std::nullopt;
}

std::vector<AssumeUTXOSnapshot> AssumeUTXORegistry::GetAllSnapshots() {
    return snapshots_;
}

bool AssumeUTXORegistry::VerifySnapshotHash(const std::filesystem::path& snapshot_path, uint32_t expected_height) {
    // Get expected snapshot metadata
    auto snapshot_opt = GetSnapshot(expected_height);
    if (!snapshot_opt.has_value()) {
        dinero::g_logger.error("VerifySnapshotHash: No snapshot registered for height " + std::to_string(expected_height));
        return false;
    }

    const auto& expected_snapshot = snapshot_opt.value();

    dinero::g_logger.info("VerifySnapshotHash: Verifying snapshot at height " + std::to_string(expected_height));
    dinero::g_logger.info("  Expected hash: " + expected_snapshot.snapshot_hash.GetHex().substr(0, 16) + "...");

    // Open snapshot file
    std::ifstream file(snapshot_path, std::ios::binary | std::ios::ate);
    if (!file) {
        dinero::g_logger.error("VerifySnapshotHash: Failed to open file: " + snapshot_path.string());
        return false;
    }

    // Get file size
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    dinero::g_logger.info("  File size: " + std::to_string(file_size) + " bytes");

    // Read entire file into memory
    // (For very large snapshots, could stream hash instead)
    std::vector<uint8_t> file_data(file_size);
    if (!file.read(reinterpret_cast<char*>(file_data.data()), file_size)) {
        dinero::g_logger.error("VerifySnapshotHash: Failed to read file");
        return false;
    }

    file.close();

    // Compute SHA256 of entire file
    dinero::crypto::CSHA256 hasher;
    hasher.Write(file_data.data(), file_data.size());
    uint256 computed_hash;
    hasher.Finalize(computed_hash.begin());

    dinero::g_logger.info("  Computed hash: " + computed_hash.GetHex().substr(0, 16) + "...");

    // Compare hashes
    if (computed_hash != expected_snapshot.snapshot_hash) {
        dinero::g_logger.error("VerifySnapshotHash: HASH MISMATCH!");
        dinero::g_logger.error("  Expected: " + expected_snapshot.snapshot_hash.GetHex());
        dinero::g_logger.error("  Computed: " + computed_hash.GetHex());
        dinero::g_logger.error("SNAPSHOT VERIFICATION FAILED - refusing to load");
        return false;
    }

    dinero::g_logger.info("VerifySnapshotHash: ✓ Hash verified - snapshot is authentic");
    dinero::g_logger.info("  Block hash: " + expected_snapshot.block_hash.GetHex().substr(0, 16) + "...");
    dinero::g_logger.info("  Height: " + std::to_string(expected_snapshot.height));
    dinero::g_logger.info("  UTXO count: " + std::to_string(expected_snapshot.utxo_count));
    dinero::g_logger.info("  Description: " + expected_snapshot.description);

    return true;
}

} // namespace consensus
} // namespace dinero
