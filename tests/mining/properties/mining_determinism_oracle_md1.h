#pragma once

#include "mining_determinism_oracle.h"

// Ring 4 Phase 4f: MD1 - Same Seed → Identical Trace
// Property: Deterministic execution from identical starting conditions

namespace mining_test {

/**
 * MD1Oracle - Same Seed → Identical Trace
 *
 * Determinism property: ∀ seed S, actions A,
 *                       Running(S, A) twice produces identical traces
 *
 * Rationale:
 * - Simulator must be fully deterministic
 * - Same seed + same actions = same events + same states
 * - No hidden entropy (time(), rand(), etc.)
 * - No order-dependent bugs
 * - Foundation for all other determinism properties
 *
 * Detection strategy:
 * - Compare trace hashes (fast path)
 * - Compare event counts
 * - Compare events element-by-element
 * - Compare states element-by-element
 * - Report first divergence point
 *
 * What MD1 catches:
 * - Hidden randomness sources
 * - Uninitialized memory reads
 * - Time-based behavior
 * - Hash map iteration order bugs
 * - Pointer-based ordering
 *
 * What MD1 requires:
 * - Identical seeds
 * - Identical action sequences
 * - No external inputs
 * - No wall-clock dependencies
 *
 * This is the STRONGEST determinism guarantee.
 * If MD1 passes, the simulator is truly deterministic.
 */
class MD1Oracle : public MiningDeterminismOracle {
public:
    MD1Oracle() = default;

    std::string name() const override;

    std::vector<DeterminismViolation> check(
        const MiningTrace& reference,
        const MiningTrace& candidate
    ) const override;

private:
    // Compare trace hashes (fast path)
    bool compareTraceHashes(
        const MiningTrace& reference,
        const MiningTrace& candidate,
        std::vector<DeterminismViolation>& violations
    ) const;

    // Compare event sequences
    bool compareEvents(
        const MiningTrace& reference,
        const MiningTrace& candidate,
        std::vector<DeterminismViolation>& violations
    ) const;

    // Compare state snapshots
    bool compareStates(
        const MiningTrace& reference,
        const MiningTrace& candidate,
        std::vector<DeterminismViolation>& violations
    ) const;

    // Helper: compare individual events
    bool eventsEqual(
        const MiningEvent& a,
        const MiningEvent& b
    ) const;

    // Helper: compare individual states
    bool statesEqual(
        const MiningState& a,
        const MiningState& b
    ) const;
};

}  // namespace mining_test
