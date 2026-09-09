// Copyright (c) 2026 Dinero Labs.
//
// ConnectBlockShieldedSection — the single shared implementation of the
// per-block shielded "connect tail": epoch-reset gate, block-level shielded
// validation, apply, and anchor-root recording. Extracted so the stateful
// BlockValidator path, the reindexer, and (later) a stateless-node reorg
// path all run byte-identical logic instead of maintaining parallel copies
// that can silently drift apart.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_accumulator.h"  // NullifierEntry (prediction oracle)
#include "consensus/shielded/nullifier_set.h"
#include "primitives/uint256.h"
#include "consensus/shielded/shielded_epoch_snapshot.h"
#include "consensus/shielded/shielded_tx.h"  // ShieldedBundle

namespace dinero::consensus::shielded {

// One shared implementation of the per-block shielded connect tail:
//   1. epoch-reset gate at reset_height (wall rule: block must be shielded-
//      empty; capture pre-reset pool into *pre_reset_snapshot_out; lossy-
//      capture check; ResetShieldedEpoch)
//   2. ValidateBlockShielded(bundles, deltas)
//   3. ApplyBlockShielded (commitments + nullifiers, block tx order)
//   4. AnchorHistory::RecordRoot once per block at/after activation_height
//      (anchors may be null on legacy validator wiring — skip 4 and refuse
//      the reset in that case, matching BlockValidator's existing behavior)
// Callers own bundle decoding and per-tx/binding validation.
bool ConnectBlockShieldedSection(
    const std::vector<ShieldedBundle>& bundles,
    const std::vector<int64_t>& deltas,
    uint32_t height,
    uint32_t cv_reset_height,
    uint32_t spend_auth_reset_height,
    uint32_t activation_height,
    CommitmentTree& tree,
    NullifierSet& nullifiers,
    AnchorHistory* anchors,  // nullable
    std::optional<ShieldedEpochSnapshot>& pre_reset_snapshot_out,
    std::string& error);

// Predict the post-block SHR1 shielded root for a CANDIDATE block without
// touching live state — the mining-side twin of ConnectBlockShieldedSection,
// kept in this file so the two cannot drift apart silently. Used by the block
// assembler to build the coinbase DNRS state commitment (state_commitment_v1),
// whose committed value is the post-block shielded state; connect-time
// validation recomputes the same value from the REAL apply and rejects a
// block whose commitment disagrees.
//
// Mirrors the connect tail exactly:
//   1. epoch-reset gate: at a reset height the pool is wiped (and the block
//      must be shielded-empty — a non-empty candidate yields nullopt, since
//      the real connect would refuse it);
//   2. every bundle output's commitment appended to the tree clone, every
//      spend's nullifier added to the entry set at `height`, block tx order;
//   3. RecordRoot(height, post-tree-root) once, at/after activation_height;
//   4. full SHR1 root from parts (tree root + size + NUL1 accumulator +
//      anchor bytes) — never the tree root alone.
//
// Pure by construction: the tree is taken by const reference and cloned, the
// entries and anchors by value. `entries` is the CURRENT nullifier set's
// content (AccumulateNullifierSet's enumeration, exposed as entries so this
// function never touches sqlite); the caller must pass a complete
// enumeration — an unreadable set must be refused by the caller, never
// passed as empty (unreadable != empty is the accumulator's core rule).
std::optional<uint256> PredictPostBlockShieldedRoot(
    const std::vector<ShieldedBundle>& bundles,
    uint32_t height,
    uint32_t cv_reset_height,
    uint32_t spend_auth_reset_height,
    uint32_t activation_height,
    const CommitmentTree& tree_in,
    std::vector<NullifierEntry> entries,
    AnchorHistory anchors);

// Compatibility overload for tests and callers modelling only one historical
// reset boundary.
bool ConnectBlockShieldedSection(
    const std::vector<ShieldedBundle>& bundles,
    const std::vector<int64_t>& deltas,
    uint32_t height,
    uint32_t reset_height,
    uint32_t activation_height,
    CommitmentTree& tree,
    NullifierSet& nullifiers,
    AnchorHistory* anchors,
    std::optional<ShieldedEpochSnapshot>& pre_reset_snapshot_out,
    std::string& error);

// Disconnect twin of ConnectBlockShieldedSection. Restores the pre-block
// shielded pool state, mirroring the undo.pre_reset_shielded_epoch (cutover)
// vs undo.pre_block_shielded_frontier (ordinary block) branching that both
// copies inside BlockValidator::DisconnectBlock implement today:
//   cutover block (pre_reset_snapshot present) -> RestoreShieldedEpoch(*pre_reset_snapshot)
//     (RollbackAbove cannot undo a reset — it only deletes rows, it can't
//     re-add the wiped nullifiers/anchors — so the full pre-reset pool is
//     restored from the captured snapshot instead. Requires anchors non-null.)
//   ordinary block (pre_block_frontier present) -> tree.DeserializeFrontier(*pre_block_frontier)
//     + nullifiers.RollbackAbove(height - 1) + anchors->RollbackAbove(height - 1)
//     (anchors rollback skipped when anchors is null)
//   neither present -> no shielded activity recorded for this block -> no-op,
//     returns true
// Does not read or depend on BlockValidator state — callers (BlockValidator's
// two DisconnectBlock copies today; a later stateless-node lightweight
// disconnect path) own locating the tree/nullifiers/anchors pointers and the
// undo record.
bool DisconnectBlockShieldedSection(
    uint32_t height,
    const std::optional<ShieldedEpochSnapshot>& pre_reset_snapshot,
    const std::optional<std::vector<uint8_t>>& pre_block_frontier,
    CommitmentTree& tree,
    NullifierSet& nullifiers,
    AnchorHistory* anchors,  // nullable; required when pre_reset_snapshot is set
    std::string& error);

}  // namespace dinero::consensus::shielded
