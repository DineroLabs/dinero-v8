#pragma once

// ============================================================================
// ChainState - Current Blockchain State
// ============================================================================
//
// Extracted from activate_best_chain.h during Phase 2 cleanup.
// Used by Phase2ActivateBestChain and chainstate_service.
//
// ============================================================================

#include "../p2p/state_transition.h"
#include <cstdint>

namespace dinero {
namespace consensus {

using p2p::Hash256;

/**
 * ChainState - Tracks the current active chain tip
 *
 * This is the global chainstate (single writer, single active tip).
 */
struct ChainState {
    Hash256 active_tip;
    uint32_t active_height;
    uint64_t active_chainwork;

    ChainState() : active_height(0), active_chainwork(0) {}
};

} // namespace consensus
} // namespace dinero
