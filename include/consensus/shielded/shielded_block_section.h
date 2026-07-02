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
#include "consensus/shielded/nullifier_set.h"
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
    uint32_t reset_height,
    uint32_t activation_height,
    CommitmentTree& tree,
    NullifierSet& nullifiers,
    AnchorHistory* anchors,  // nullable
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
