#pragma once

#include "primitives/uint256.h"
#include "consensus/state_commitment.h"  // IsStateCommitmentActive — the single dormancy authority
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
// v4 adds the shielded-pool bootstrap section (frontier + anchor history +
// nullifiers). Without it a snapshot-bootstrapped node starts with an EMPTY
// shielded commitment tree, so the first post-snapshot shielded spend fails
// ShieldedValidationError::AnchorInvalid and the chain wedges.
constexpr uint32_t SNAPSHOT_VERSION_V4 = 4;
// v5 = v4 + the state-commitment binding proof section (base coinbase +
// merkle branch). Mandatory for any base at or above
// state_commitment_activation_height; see snapshot_binding.h.
constexpr uint32_t SNAPSHOT_VERSION_V5 = 5;

// ===========================================================================
// Snapshot container format policy (fail closed)
// ===========================================================================
//
// V2 is DEPRECATED AND REJECTED UNCONDITIONALLY. This is a deprecation, not a
// validation gate: there is no V2 file that can pass. A V2 container carries no
// utreexo section at all, so LoadSnapshot would have to RECONSTRUCT the forest
// by sorting UTXOs by OutPoint -- which is not chronological insertion order,
// as the reconstruction site itself acknowledges. Nothing downstream can repair
// a wrong leaf ORDER, so accepting V2 means importing a forest whose commitment
// cannot match the base block's utreexo_root. Do not reintroduce it as a
// conditional check; that would imply a valid V2 file exists.
//
// V3 carries a utreexo section but NO shielded section. Below shielded
// activation that is harmless. At or after it, a V3-bootstrapped node starts
// with an empty shielded commitment tree and wedges on the first post-snapshot
// shielded spend -- the same reasoning that removed the v3 anchors from
// AssumeUTXORegistry.
//
// V4 is the supported production format. Current exporters emit V4 and the
// registered anchors are V4, so this removes legacy compatibility without
// disturbing the production bootstrap path.
enum class SnapshotFormatVerdict {
    Accept,
    RejectV2Deprecated,
    RejectV3PostShieldedActivation,
    RejectV4PostStateCommitmentActivation,
    RejectUnknownVersion,
};

// Pure, side-effect-free policy so the boundary can be tested exhaustively
// without standing up a chainstate. Both activation heights are passed in
// rather than read from Params() so tests can sweep them INDEPENDENTLY —
// including the mixed quadrants (shielded active + commitment dormant, and
// the reverse), which is where a bug coupling the two activations would hide.
//
// TWO INDEPENDENT POLICIES, ONE DECISION POINT:
//   - the v3 rule keys on shielded-TRANSACTION activation
//     (`shielded_activation_height`);
//   - the v4 rule keys on snapshot-TRUST activation
//     (`state_commitment_activation_height`), decided at the SNAPSHOT BASE:
//     below activation v4 stays acceptable; at or above it, only v5 (which
//     carries the coinbase + merkle-branch binding proof) may be accepted.
//   Keying either rule on the other's height would silently couple two
//   policies that must be able to move independently.
//
// `block_height` here is the snapshot file's own claimed base height —
// ATTACKER-CONTROLLED input, not chain-derived. The degenerate claimed height
// UINT32_MAX therefore matters: with a real activation set, `>=` holds and
// the policy REJECTS — it fails closed by DIRECTION. That, not
// "unreachability", is the safety argument (an earlier version of this
// comment claimed no height satisfies `>=` in the dormant case; the policy
// test pins the opposite). With activation dormant, the v4 rule uses the
// IsStateCommitmentActive sentinel guard, so dormant means dormant at every
// claimed height with no special-casing.
inline SnapshotFormatVerdict EvaluateSnapshotFormat(
        uint32_t version,
        uint32_t block_height,
        uint32_t shielded_activation_height,
        uint32_t state_commitment_activation_height) {
    if (version == SNAPSHOT_VERSION_V2) {
        return SnapshotFormatVerdict::RejectV2Deprecated;
    }
    if (version == SNAPSHOT_VERSION_V3) {
        return (block_height >= shielded_activation_height)
                   ? SnapshotFormatVerdict::RejectV3PostShieldedActivation
                   : SnapshotFormatVerdict::Accept;
    }
    if (version == SNAPSHOT_VERSION_V4) {
        return IsStateCommitmentActive(
                   block_height, state_commitment_activation_height)
                   ? SnapshotFormatVerdict::RejectV4PostStateCommitmentActivation
                   : SnapshotFormatVerdict::Accept;
    }
    if (version == SNAPSHOT_VERSION_V5) {
        // Acceptable at every height: v5 is v4 plus the binding proof, and
        // carrying a proof below activation is harmless (the proof is then
        // verified opportunistically, enforced once active). The format
        // policy only answers "may this container be parsed here" — the
        // proof's own verification chain lives in snapshot_binding.h.
        return SnapshotFormatVerdict::Accept;
    }
    return SnapshotFormatVerdict::RejectUnknownVersion;
}
constexpr uint32_t SNAPSHOT_VERSION = SNAPSHOT_VERSION_V4;

// Optional v3 section carrying the accumulator bootstrap payload.
constexpr uint32_t SNAPSHOT_V3_UTREEXO_MAGIC = 0x58525455;  // "UTRX" in little-endian
constexpr uint32_t SNAPSHOT_V3_UTREEXO_SECTION_VERSION = 1;
constexpr uint64_t SNAPSHOT_V3_MAX_FOREST_BYTES = (1ULL << 30);  // 1 GiB hard cap

// Optional v4 section carrying the shielded-pool bootstrap payload. Appended
// after the v3 utreexo section and covered by the same trailing SHA256 checksum.
constexpr uint32_t SNAPSHOT_V4_SHIELDED_MAGIC = 0x444C4853;  // "SHLD" in little-endian
constexpr uint32_t SNAPSHOT_V4_SHIELDED_SECTION_VERSION = 1;
constexpr uint64_t SNAPSHOT_V4_MAX_SHIELDED_BYTES = (1ULL << 30);  // 1 GiB hard cap

// v5 section carrying the state-commitment binding proof: the base block's
// coinbase transaction plus its merkle branch to the base header's tx merkle
// root. Appended after the v4 shielded section, covered by the same trailing
// checksum. Layout on disk:
//   [magic u32] [section_version u32] [coinbase_bytes u64] [branch_count u32]
//   [coinbase serialized WithWitness] [branch_count × 32-byte hashes]
constexpr uint32_t SNAPSHOT_V5_BINDING_MAGIC = 0x444E4942;  // "BIND" in little-endian
constexpr uint32_t SNAPSHOT_V5_BINDING_SECTION_VERSION = 1;
// A coinbase is small; 4 MiB is generous headroom and refuses attacker-sized
// payloads before allocation. Branch depth is capped at 32 (2^32 leaves).
constexpr uint64_t SNAPSHOT_V5_MAX_COINBASE_BYTES = (4ULL << 20);
constexpr uint32_t SNAPSHOT_V5_MAX_BRANCH_HASHES = 32;

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
    uint64_t timestamp;       // v4: export time; v5: base block timestamp (new exports)
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
 * Optional v4 section metadata for shielded-pool bootstrap.
 *
 * Carries the state needed to validate post-snapshot shielded transactions:
 *   - the shielded commitment-tree frontier (CommitmentTree::SerializeFrontier)
 *   - the anchor-history window      (AnchorHistory::SerializeBytes)
 *   - the nullifier set              (NullifierSet::SerializeContent; 0 = omitted)
 * Header is followed by the three payloads in that order.
 */
struct SnapshotShieldedSection {
    uint32_t magic;                 // SNAPSHOT_V4_SHIELDED_MAGIC
    uint32_t version;               // SNAPSHOT_V4_SHIELDED_SECTION_VERSION
    uint64_t frontier_bytes;        // serialized commitment-tree frontier size
    uint64_t anchor_history_bytes;  // serialized anchor-history size
    uint64_t nullifier_bytes;       // serialized nullifier-set size (0 if omitted)
    uint64_t reserved;              // reserved for future fields
    uint256  commitment_root;       // shielded tree root at snapshot base (sanity)

    SnapshotShieldedSection()
        : magic(SNAPSHOT_V4_SHIELDED_MAGIC)
        , version(SNAPSHOT_V4_SHIELDED_SECTION_VERSION)
        , frontier_bytes(0)
        , anchor_history_bytes(0)
        , nullifier_bytes(0)
        , reserved(0)
        , commitment_root()
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
