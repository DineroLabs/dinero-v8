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

}  // namespace dinero::consensus::shielded
