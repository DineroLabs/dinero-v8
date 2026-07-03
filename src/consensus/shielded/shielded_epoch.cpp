// Copyright (c) 2026 Dinero Labs.
//
// Shielded epoch reset (hard-fork cutover). See shielded_epoch.h.
#include "consensus/shielded/shielded_epoch.h"

namespace dinero {
namespace consensus {
namespace shielded {

void ResetShieldedEpoch(CommitmentTree& tree,
                        AnchorHistory& anchors,
                        NullifierSet& nullifiers) {
    // Discard the entire pre-cutover pool. Assigning a fresh CommitmentTree makes
    // the result byte-identical to a from-scratch-empty pool (deterministic
    // across nodes → identical cutover shieldedStateHash). The cleared anchor
    // history is what renders every pre-cutover note unspendable: a spend must
    // match a new-epoch anchor, and none survive here.
    tree = CommitmentTree{};
    anchors.Clear();
    nullifiers.Clear();
}

ShieldedEpochSnapshot CaptureShieldedEpoch(const CommitmentTree& tree,
                                           const AnchorHistory& anchors,
                                           const NullifierSet& nullifiers) {
    return ShieldedEpochSnapshot{
        tree.SerializeFrontier(),
        anchors.SerializeBytes(),
        nullifiers.SerializeContent(),
    };
}

bool RestoreShieldedEpoch(const ShieldedEpochSnapshot& snapshot,
                          CommitmentTree& tree,
                          AnchorHistory& anchors,
                          NullifierSet& nullifiers) {
    if (!tree.DeserializeFrontier(snapshot.tree_frontier.data(),
                                  snapshot.tree_frontier.size())) {
        return false;
    }
    if (anchors.DeserializeBytes(snapshot.anchor_history) !=
        AnchorHistory::IoResult::Ok) {
        return false;
    }
    if (!nullifiers.DeserializeContent(snapshot.nullifiers)) {
        return false;
    }
    return true;
}

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
