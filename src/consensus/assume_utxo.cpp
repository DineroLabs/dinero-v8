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
    // Mainnet height 52287 v4 trust anchor (generated 2026-06-28 by dumptxoutset
    // on an rsync'd copy of the production full-genesis-validated 126G archival
    // node, Dell /var/lib/dinero, sitting at the canonical fleet tip 52287).
    // Bundled in the desktop wallet installers so fresh GUI
    // users fast-sync instead of syncing from genesis. Format: v4 UXTO + UTRX
    // (Utreexo) + SHLD (shielded pool) snapshot — carries the shielded
    // commitment-tree frontier + anchor history + nullifier set so a
    // snapshot-bootstrapped node restores correct shielded state (tree_size=10,
    // nullifier_count=4, root 6c45517648d707f4…) instead of an empty tree that
    // wedges on the first post-snapshot shielded spend.
    // Snapshot file: utxo-snapshot-52287.dat (19,085,326 bytes)
    //
    // Cross-validated against fleet consensus at the dump height (the only thing
    // that makes a bootstrap source shippable — provenance is irrelevant once the
    // content matches independently-validated full nodes):
    //   • tip block hash == fleet canonical 000000739c14918aae… @ 52287
    //   • daemon.shieldedstatehash == fleet bc1260e9f704d779… (shielded consensus)
    //   • full-genesis-validated source ⇒ canonical UTXO set (154475 coins)
    //
    // ⚠️ 52287 SUPERSEDES the 52241 v4 anchor (fresher tip, fewer blocks for a new
    // node to replay). Both are v4+SHLD; the older 52241 is retired only because a
    // closer-to-tip anchor is strictly better for fast-sync. As always: production
    // AssumeUTXO anchors after the shielded activation height (8650) MUST be v4+
    // and include the SHLD section — never re-add a v3/no-shielded anchor.
    AssumeUTXOSnapshot(
        "48f7672cc855c83cd8968fddab85a87d4cd8c41aa8562c91bff475a318db399c",
        "000000739c14918aae1985948b1d800cbab8473edf117c155ba9ada186cba71e",
        52287,
        "0x000000000000000000000000000000000000000000000000000005bdb4f5fd56",
        154475,
        "Mainnet height 52287 v4 trust anchor (UXTO+UTRX+SHLD)"
    ),
    // Mainnet height 65300 v4 trust anchor (generated 2026-07-18 by dumptxoutset
    // on the production fleet node (na, /var/lib/dinero) at the canonical tip,
    // now buried 139+ blocks below the live tip). Bundled in the desktop wallet
    // installers so fresh GUI users fast-sync from 65300 instead of the older
    // 52287 (fewer blocks to replay). Format: v4 UXTO+UTRX(Utreexo)+SHLD.
    // Snapshot file: utxo-snapshot-65300.dat (23,935,983 bytes).
    //
    // Cross-validated against fleet consensus at the dump height:
    //   • tip block hash == fleet canonical 00000098ccb1c3a0… @ 65300
    //     (na/sj/eu1 agreed on this best hash at height 65300)
    //   • utreexo_root 1202a3a97807c5cf55b6df929b80d77eac7445b3e2dc6643fc65d9b054cad9f0
    //   • full-genesis-validated source ⇒ canonical UTXO set (193812 coins)
    //
    // ⚠️ 65300 SUPERSEDES 52287 as the preferred bundled fast-sync anchor
    // (closer to tip). 52287 is retained for older bundles/manifests already in
    // the field. Both are v4+SHLD — never re-add a v3/no-shielded anchor.
    AssumeUTXOSnapshot(
        "d7f6c0a53a5429ed293200e5d30e19cf23352ea2fd3ef44c35eda9ae23c6e7c6",
        "00000098ccb1c3a0204ea9fb077be4975146be5a95ec2865ebde9cd0644462ed",
        65300,
        "0x0000000000000000000000000000000000000000000000000000068b62500bc1",
        193812,
        "Mainnet height 65300 v4 trust anchor (UXTO+UTRX+SHLD)"
    ),
    // Mainnet height 73035 v4 trust anchor — the snapshot SHIPPED INSIDE
    // DineroDPI.app. Registering it upgrades that artifact from
    // manifest-verified to content-pinned: LoadSnapshot() will additionally
    // require the file's SHA-256 and base block hash to match these constants
    // before importing a single UTXO.
    //
    // ⚠️ EXACT-FILE PINNING IS INTENTIONAL. After this entry, a REGENERATED
    // snapshot at height 73035 — even a perfectly valid one — is REJECTED,
    // because its bytes will differ. That is the desired behaviour: the shipped
    // artifact is the trusted anchor. A regenerated snapshot must either get its
    // own reviewed registry entry or stay on the manifest-only path at an
    // unregistered height.
    //
    // PROVENANCE — every field below was verified from the artifact itself, not
    // transcribed. Reproduce with:
    //
    //   F=/Applications/DineroDPI.app/Contents/Resources/mainnet-snapshot.dat
    //   shasum -a 256 "$F"        # -> 0a98ab1b…e8a4
    //   ls -l "$F"                # -> 27015629 bytes
    //   python3 -c 'import struct,binascii;d=open("'"$F"'","rb").read(52);\
    //     print(hex(struct.unpack_from("<I",d,0)[0]),      # magic 0x4F545855 "UTXO"
    //           struct.unpack_from("<I",d,4)[0],           # version    -> 4
    //           struct.unpack_from("<I",d,40)[0],          # height     -> 73035
    //           struct.unpack_from("<Q",d,44)[0],          # utxo_count -> 218833
    //           binascii.hexlify(d[8:40][::-1]).decode())' # block_hash -> 0000004ba0…2756
    //
    // Cross-checked against the bundled manifest
    // (mainnet-snapshot.dat.manifest.json), which independently carries the same
    // sha256, height, block_hash and byte count; generated 20260726T043539Z by
    // "EU1 dinero-snapshot-publish (automated, self-check gated)".
    //
    // block_hash and chainwork additionally confirmed against the CANONICAL
    // CHAIN via a live fleet node:
    //   blockchain.getblockhash   [73035] -> 0000004ba0e611b00543c4210f29e7b72d91fc35007c1bad5c13f7b3a06c2756
    //   blockchain.getblockheader        -> chainwork 0x…06e15b611f17
    //
    // CI CANNOT re-verify the hash: the 27 MB artifact is not in this
    // repository. The commands above are the permanent provenance record. The
    // accompanying tests check the constants for self-consistency and against
    // the manifest's published values — they do NOT hash the artifact.
    //
    // Format: v4 UXTO + UTRX + SHLD, so it satisfies the no-v3-anchor rule at
    // the top of this list (height 73035 is far above shielded activation 8650).
    AssumeUTXOSnapshot(
        "0a98ab1bd544d333afae7c8d2b42b0a910fb5e7fcdefd40642c6d3e0c6aae8a4",
        "0000004ba0e611b00543c4210f29e7b72d91fc35007c1bad5c13f7b3a06c2756",
        73035,
        "0x000000000000000000000000000000000000000000000000000006e15b611f17",
        218833,
        "Mainnet height 73035 v4 trust anchor (UXTO+UTRX+SHLD) - shipped DineroDPI artifact"
    ),
    // Mainnet height 84131 v4 trust anchor. This is the preferred snapshot for
    // fresh v8.1.2 installations; 73035 remains registered and is shipped as a
    // fallback so an interrupted older AssumeUTXO lifecycle can restart against
    // its exact original base instead of being forced onto this newer one.
    //
    // Provenance (2026-08-08): the signed EU1 publisher manifest was fetched
    // independently and verified with DineroDPI's embedded Ed25519 publisher
    // key. The artifact then passed the real offline LoadSnapshot/loadtxoutset
    // path against a header store containing the canonical base header. That
    // path verified the whole-file checksum, manifest-pinned file and block hashes,
    // Utreexo section root against the base header, deserialized forest root and
    // leaf count, and the restored v4 shielded commitment root/nullifier payload.
    // The daemon had zero peers throughout the import.
    //
    // Reproduce the artifact fields with:
    //   shasum -a 256 dinero-assumeutxo-84131-v4.dat
    //   python3 -c 'import struct,binascii;d=open("dinero-assumeutxo-84131-v4.dat","rb").read(52);\
    //     print(hex(struct.unpack_from("<I",d,0)[0]), struct.unpack_from("<I",d,4)[0],\
    //       struct.unpack_from("<I",d,40)[0], struct.unpack_from("<Q",d,44)[0],\
    //       binascii.hexlify(d[8:40][::-1]).decode())'
    // Expected: magic 0x4f545855, version 4, height 84131, 252129 UTXOs,
    // block 0000000023974d67...c6792123; file size 31,122,296 bytes.
    AssumeUTXOSnapshot(
        "7defc1897055ba24a92985e30ce35123181debc272c86e5fd2d190e436802845",
        "0000000023974d67c7a1a5dc04b7d63764b0f41b756796330615d39bc6792123",
        84131,
        "0x00000000000000000000000000000000000000000000000000002733d734bd86",
        252129,
        "Mainnet height 84131 v4 trust anchor (UXTO+UTRX+SHLD) - signed EU1 artifact"
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
