#pragma once
// Pure acceptance policy for side-chain (fork) blocks. Kept header-only and
// backed by explicit active ancestry, so a future checkpoint cannot freeze IBD.
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
#include <optional>
#include "consensus/active_chain_ancestry.h"

namespace dinero::consensus {

// Highest checkpointed height, or 0 when there are no checkpoints.
inline uint32_t LastCheckpointHeight(const std::map<uint32_t, std::string>& checkpoints) {
    return checkpoints.empty() ? 0u : checkpoints.rbegin()->first;
}

// Return the established checkpoint violated by this block, if any. A
// configured future checkpoint does not prove that today's active branch is
// correct. During IBD a competing branch may contain that checkpoint, and its
// bodies must remain eligible even while the old branch is still active.
// Only trust a checkpoint whose exact hash is present in active-tip ancestry.
// A missing ancestry link is not proof of a conflict.
inline std::optional<uint32_t> ForkPriorToEstablishedCheckpoint(
    uint32_t height, const uint256& block_hash,
    const std::map<uint32_t, std::string>& checkpoints,
    const CBlockIndex* active_tip) {
    if (height > LastCheckpointHeight(checkpoints)) return std::nullopt;
    const CBlockIndex* cursor = active_tip;
    for (auto cp = checkpoints.rbegin(); cp != checkpoints.rend(); ++cp) {
        if (cp->first == 0 || height > cp->first) break;
        while (cursor && cursor->height > cp->first) cursor = cursor->pprev;
        if (!cursor) break;
        if (cursor->height != cp->first || cursor->hash.GetHex() != cp->second) continue;
        uint256 expected;
        if (GetActiveChainHashAtHeight(cursor, height, expected) && expected != block_hash)
            return cp->first;
        return std::nullopt;
    }
    return std::nullopt;
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
