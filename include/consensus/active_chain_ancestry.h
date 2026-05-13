#pragma once

#include "consensus/block_index.h"
#include "primitives/uint256.h"

#include <cstdint>

namespace dinero {
namespace consensus {

/**
 * Resolve the active-chain block hash at a given height by walking tip ancestry.
 *
 * This deliberately ignores persisted height indexes because they can lag or
 * remain stale across reorgs while the active tip/pprev chain is already
 * authoritative in memory.
 */
inline bool GetActiveChainHashAtHeight(
    const CBlockIndex* active_tip,
    uint32_t height,
    uint256& out_hash
) {
    if (!active_tip) {
        return false;
    }
    if (height > static_cast<uint32_t>(active_tip->height)) {
        return false;
    }

    const CBlockIndex* cursor = active_tip;
    while (cursor && cursor->height > static_cast<int>(height)) {
        cursor = cursor->pprev;
    }
    if (!cursor || cursor->height != static_cast<int>(height)) {
        return false;
    }

    out_hash = cursor->hash;
    return true;
}

}  // namespace consensus
}  // namespace dinero
