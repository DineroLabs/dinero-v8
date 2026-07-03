// Copyright (c) 2026 Dinero Labs.
//
// Shielded epoch reset (hard-fork cutover).
//
// At the shielded_epoch_reset_height, the shielded pool is reset to a fresh,
// empty epoch: the note-commitment tree, the anchor history, and the nullifier
// set are all discarded. This makes every pre-cutover note UNSPENDABLE — a spend
// must prove membership against a new-epoch anchor, and no pre-cutover anchor
// survives the reset. Combined with cv-binding activating at the same height,
// this closes the [input_binding, cv) mint window by discarding the weak pool
// rather than carrying it forward. Transparent/UTXO state is untouched.
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_epoch_snapshot.h"

namespace dinero {
namespace consensus {
namespace shielded {

// Sentinel: no shielded epoch reset scheduled (dormant on this network).
inline constexpr uint32_t kShieldedEpochResetDormant = std::numeric_limits<uint32_t>::max();

// True iff `height` is exactly the shielded epoch reset boundary. Pure — the
// caller passes the active chain's reset height (Params().shielded_epoch_reset_height).
inline bool IsShieldedEpochResetHeight(uint32_t height, uint32_t reset_height) {
    return reset_height != kShieldedEpochResetDormant && height == reset_height;
}

// The reset MUST coincide with cv-binding activation: cv-binding is enforced from
// block 1 of the new epoch, and no window may exist where balance is enforced
// over cv while cv is unbound. Both dormant (UINT32_MAX) trivially satisfies this.
inline bool ShieldedEpochParamsConsistent(uint32_t reset_height, uint32_t cv_binding_height) {
    return reset_height == cv_binding_height;
}

// The reset block (cutover) MUST be shielded-empty. At exactly the reset height
// the old pool is being discarded and the new epoch is empty: a shielded SPEND
// has no valid new-epoch anchor to prove against, and a shielded OUTPUT would
// race the reset. Enforcing a clean wall — reject ANY shielded tx at exactly the
// reset height — removes the entire ordering/race question (the reset has
// nothing to contend with) and closes the spend-across-cutover hole that would
// otherwise reopen the very mint window this fork exists to shut. New-epoch
// shielded activity begins at reset_height + 1. Callers reject a block when it
// contains a shielded tx and this returns false.
inline bool ShieldedTxAllowedAtHeight(uint32_t height, uint32_t reset_height) {
    return !IsShieldedEpochResetHeight(height, reset_height);
}

// Reset the shielded pool state to a fresh empty epoch. Deterministic: the
// result must be byte-identical to a from-scratch-empty pool on every node
// (the cutover block's shieldedStateHash depends on it).
void ResetShieldedEpoch(CommitmentTree& tree,
                        AnchorHistory& anchors,
                        NullifierSet& nullifiers);

// ShieldedEpochSnapshot lives in shielded_epoch_snapshot.h (included above) so
// BlockUndo can carry it without the heavy shielded headers.

// Serialize the current pool state into a snapshot (before applying the reset).
ShieldedEpochSnapshot CaptureShieldedEpoch(const CommitmentTree& tree,
                                           const AnchorHistory& anchors,
                                           const NullifierSet& nullifiers);

// Restore a captured snapshot into the pool (reorg disconnect across the reset).
// Returns false if any of the three structures fails to deserialize; on failure
// the pool may be partially restored, so the caller must treat it as fatal.
bool RestoreShieldedEpoch(const ShieldedEpochSnapshot& snapshot,
                          CommitmentTree& tree,
                          AnchorHistory& anchors,
                          NullifierSet& nullifiers);

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
