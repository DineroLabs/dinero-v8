// Copyright (c) 2026 Dinero Labs.
//
// ShieldedEpochSnapshot — serialized snapshot of the full pre-cutover shielded
// pool. Split into its own tiny header (just three byte-vectors, no sqlite /
// commitment-tree / anchor-history dependencies) so BlockUndo can carry it in
// its persisted undo record without pulling the heavy shielded headers into
// every translation unit that includes block_undo.h.
#pragma once

#include <cstdint>
#include <vector>

namespace dinero {
namespace consensus {
namespace shielded {

// Captured before an epoch reset so a reorg that disconnects across the reset
// can restore the old epoch exactly. The old pool is tiny (~4 notes), so
// carrying the whole nullifier list here is cheap.
struct ShieldedEpochSnapshot {
    std::vector<uint8_t> tree_frontier;   // CommitmentTree::SerializeFrontier
    std::vector<uint8_t> anchor_history;  // AnchorHistory::SerializeBytes
    std::vector<uint8_t> nullifiers;      // NullifierSet::SerializeContent
};

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
