#pragma once

#include "mining_determinism_oracle.h"

// Ring 4 Phase 4f: MD2 - Restart Replay Determinism
// Property: Crash/restart cycles are deterministic

namespace mining_test {

/**
 * MD2Oracle - Restart Replay Determinism
 *
 * Determinism property: ∀ seed S, actions A with crash/restart,
 *                       Replaying from restart produces identical suffix
 *
 * Rationale:
 * - Crashes must be deterministic transitions
 * - Restarts must not introduce divergence
 * - Replay from checkpoint must be stable
 * - Recovery must be reproducible
 *
 * Detection strategy:
 * - Find first CRASH or RESTART event
 * - Compare trace suffix after restart boundary
 * - Allow prefix to differ (before restart)
 * - Enforce suffix equality (after restart)
 *
 * What MD2 catches:
 * - Restart nondeterminism (re-seeding, time-based recovery)
 * - Hidden state resets during recovery
 * - Order-dependent recovery paths
 * - Partial state corruption after crash
 *
 * What MD2 requires:
 * - Identical seeds
 * - Identical crash/restart schedule
 * - Deterministic recovery logic
 * - No external state during recovery
 *
 * This guarantees: "A crash is just another deterministic transition."
 * Essential for replay debugging and Phase 4g persistence.
 */
class MD2Oracle : public MiningDeterminismOracle {
public:
    MD2Oracle() = default;

    std::string name() const override;

    std::vector<DeterminismViolation> check(
        const MiningTrace& reference,
        const MiningTrace& candidate
    ) const override;

private:
    // Find restart boundary in trace
    size_t findRestartBoundary(const MiningTrace& trace) const;

    // Compare trace suffix after restart
    bool compareSuffix(
        const MiningTrace& reference,
        const MiningTrace& candidate,
        size_t restart_index,
        std::vector<DeterminismViolation>& violations
    ) const;
};

}  // namespace mining_test
