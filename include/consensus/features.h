#pragma once

/**
 * @file features.h
 * @brief Authoritative consensus feature constants
 *
 * This file is the SINGLE SOURCE OF TRUTH for consensus feature enablement.
 *
 * Rules for this file:
 * 1. Only consensus-critical feature flags belong here
 * 2. Each constant must have a clear, auditable meaning
 * 3. Changes to this file require consensus review
 * 4. No macros - only constexpr constants
 * 5. No conditional compilation - runtime checks only
 *
 * Feature Constants:
 * - CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO: Delta-based undo for Utreexo reorgs
 */

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════════
// UTREEXO CONSENSUS FEATURES
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Phase 4 Delta-Based Undo Implementation Status
 *
 * When TRUE:
 * - Utreexo uses O(changes) delta undo, not O(forest) snapshots
 * - Phase 3 height limits are disabled (no longer needed)
 * - Utreexo can be activated at any height without performance concerns
 *
 * When FALSE:
 * - Phase 3 limits apply (max height 100,000)
 * - Full forest serialization on every block
 * - NOT suitable for production mainnet
 *
 * This constant was set to TRUE after audit confirmed:
 * - Delta storage is O(changes), not O(forest)
 * - Reorg depth > 1 works without full forest reserialization
 * - Memory growth is bounded across long reorgs
 * - Disk undo is deterministic and replayable
 *
 * Audit date: 2026-01-20
 * Audit scope: Phase 4 Delta Undo Truth Audit
 */
constexpr bool CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO = true;

// ═══════════════════════════════════════════════════════════════════════════════
// COMPILE-TIME SAFETY ASSERTIONS
// ═══════════════════════════════════════════════════════════════════════════════

// No feature can be "partially true" - this file is authoritative
static_assert(CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO == true ||
              CONSENSUS_UTREEXO_PHASE4_DELTA_UNDO == false,
    "Feature constants must be explicitly true or false");

} // namespace consensus
} // namespace dinero
