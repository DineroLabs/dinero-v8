#pragma once
// Pure acceptance policy for side-chain (fork) blocks. Kept header-only and
// dependency-free so the rules are unit-testable without a chainstate.
//
// Background (dinero-v8 #803): a peer fed SJ blocks from an abandoned
// April-era branch (heights 1988/2086/2234). Each was accepted as a side-chain
// block and the acceptor's fork-aware utreexo overlay then walked main-chain
// undo data from tip 114173 down to the fork parent (~112k undo reads plus a
// tip-to-height pointer walk per height) under the block-ingress lock: ~40
// minutes per block during which the node processed nothing else.

#include <cstdint>
#include <map>
#include <string>

namespace dinero::consensus {

// Highest checkpointed height, or 0 when there are no checkpoints.
inline uint32_t LastCheckpointHeight(const std::map<uint32_t, std::string>& checkpoints) {
    return checkpoints.empty() ? 0u : checkpoints.rbegin()->first;
}

// Bitcoin Core's "bad-fork-prior-to-checkpoint": a NEW block at or below the
// last checkpoint height cannot be on the checkpointed chain (those blocks are
// already known), so it can never activate and must be rejected before any
// expensive work. `block_is_canonical_at_height` lets a re-delivered main-chain
// block pass through to the ordinary duplicate handling.
inline bool ForksPriorToLastCheckpoint(uint32_t height,
                                       const std::map<uint32_t, std::string>& checkpoints,
                                       bool block_is_canonical_at_height) {
    const uint32_t last = LastCheckpointHeight(checkpoints);
    if (last == 0 || height > last) return false;
    return !block_is_canonical_at_height;
}

// The fork-aware utreexo root pre-check for a side-chain block costs
// O(tip - parent) undo reads. It is only a consistency pre-check: the branch is
// fully validated per block by the ConnectTip walk if it ever becomes the best
// candidate. Skip the pre-check for forks deeper than this many blocks.
inline constexpr uint32_t kMaxForkAwareOverlayDepth = 1000;

inline bool ForkAwareOverlayWithinDepth(uint32_t tip_height, uint32_t parent_height) {
    if (parent_height >= tip_height) return true;
    return (tip_height - parent_height) <= kMaxForkAwareOverlayDepth;
}

}  // namespace dinero::consensus
