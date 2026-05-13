#pragma once

#include <cstdint>
#include "consensus/features.h"  // Authoritative feature constants

/**
 * @file utreexo_phase_guard.h
 * @brief Phase 3 safety guards for Utreexo forest serialization
 *
 * HISTORICAL CONTEXT:
 *
 * Phase 3 Implementation (RETIRED):
 * - Serialized ENTIRE forest on every block (O(n) per block)
 * - Safe and correct, but catastrophic for mainnet at scale
 * - Height limit of 100,000 blocks prevented accidental production deployment
 *
 * Phase 4 Implementation (ACTIVE):
 * - Delta-based Utreexo undo (O(changes) per block)
 * - Only serializes forest CHANGES, not entire state
 * - Suitable for production mainnet at any scale
 *
 * The Phase 3 guard is now controlled by CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO
 * in consensus/features.h. When Phase 4 is enabled (true), the height limit
 * is bypassed and Utreexo can operate at any height.
 */

namespace dinero {
namespace consensus {

// Phase 3 maximum height (historical - only applies if Phase 4 disabled)
constexpr uint32_t UTREEXO_PHASE3_MAX_HEIGHT = 100000;

// Compile-time assertion: Phase 4 must be enabled for production
// This prevents accidentally disabling Phase 4 without understanding consequences
static_assert(CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO,
    "CONSENSUS VIOLATION: Phase 4 delta-based undo is required for production. "
    "Phase 3 uses full forest serialization per block (O(n)), which is "
    "catastrophic for mainnet. Do not disable CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO.");

/**
 * @brief Runtime guard for Utreexo forest operations
 * @param height Block height
 * @return true if Utreexo operations are allowed at this height
 *
 * With Phase 4 enabled (CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO = true):
 *   - Returns true for ALL heights (no limit)
 *
 * With Phase 4 disabled (CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO = false):
 *   - Returns true only for height <= UTREEXO_PHASE3_MAX_HEIGHT
 *   - Prevents catastrophic performance on large chains
 */
inline bool IsUtreexoSnapshotAllowed(uint32_t height) {
    if constexpr (!CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO) {
        // Phase 3 mode: enforce height limit
        if (height > UTREEXO_PHASE3_MAX_HEIGHT) {
            return false;
        }
    }
    // Phase 4 mode: no height limit
    return true;
}

} // namespace consensus
} // namespace dinero
