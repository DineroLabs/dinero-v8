#pragma once
// Header-chain convergence: does the active chain match the best known header
// chain? (issue #439)
//
// Deliberately a tiny standalone header. The rule is pure and the snapshot is a
// plain value type, so the behavior-matrix test
// (tests/consensus/test_sync_snapshot_matrix.cpp) exercises the PRODUCTION types
// and methods directly, without dragging in RocksDB and the rest of the daemon.
//
// Convergence is ONE of four distinct sync states and must not be conflated with
// the others — see docs/architecture/sync-state-behavior-matrix.md:
//   1. header convergence   (this file)
//   2. initial-download policy   ChainstateService::IsInIBD()
//   3. service readiness         ChainstateService::AreServicesReady()
//   4. AssumeUTXO readiness      assumeutxo + IsBackgroundValidationComplete()

#include "primitives/uint256.h"

#include <cstdint>

namespace dinero {
namespace consensus {

enum class HeaderConvergence {
    Unknown,   // A required input was missing — NEVER treat as synced
    Mismatch,  // Active tip hash != best header hash
    Converged  // Active tip hash == best header hash
};

/**
 * @brief The convergence rule, as a pure function.
 *
 * Fails closed: any missing input yields Unknown.
 *
 * Compares HASHES, never heights. Note the deliberate naming: hash inequality
 * establishes only that the two chains DIFFER — it cannot establish which side
 * is ahead. The best header chain may be ahead, momentarily behind mid-reorg, or
 * on an equal-height fork. Hence `Mismatch`, not "HeadersAhead".
 */
inline HeaderConvergence ComputeHeaderConvergence(bool has_best_header,
                                                  const uint256& best_header_hash,
                                                  bool has_active_tip,
                                                  const uint256& active_tip_hash) {
    if (!has_best_header || !has_active_tip) {
        return HeaderConvergence::Unknown;
    }
    return (best_header_hash == active_tip_hash) ? HeaderConvergence::Converged
                                                 : HeaderConvergence::Mismatch;
}

/**
 * @brief Value snapshot of header-chain sync facts.
 *
 * Lives here rather than nested in ChainstateService so tests can construct one
 * and call IsConverged() — the production method itself, not a re-implementation
 * of it.
 */
struct SyncSnapshot {
    bool              has_best_header = false;
    uint256           best_header_hash;
    uint32_t          best_header_height = 0;

    bool              has_active_tip = false;
    uint256           active_tip_hash;
    uint32_t          active_tip_height = 0;

    HeaderConvergence convergence = HeaderConvergence::Unknown;

    // True ONLY for an explicit Converged. Fails closed, so a missing selector,
    // best header or active tip can never read as synced.
    bool IsConverged() const { return convergence == HeaderConvergence::Converged; }

    // Derive `convergence` from the current inputs.
    void RecomputeConvergence() {
        convergence = ComputeHeaderConvergence(has_best_header, best_header_hash,
                                               has_active_tip, active_tip_hash);
    }
};

/**
 * A hash mismatch with both inputs present is a recoverable header/body gap:
 * callers may continue using the validated active tip. Unknown/missing inputs
 * fail closed and must never enable continuity.
 */
inline bool CanServeValidatedTipContinuity(const SyncSnapshot& snapshot) {
    return snapshot.has_active_tip && snapshot.has_best_header &&
           snapshot.convergence == HeaderConvergence::Mismatch;
}

}  // namespace consensus
}  // namespace dinero
