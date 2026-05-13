#pragma once

#include "../framework/mining_trace.h"
#include "../framework/mining_types.h"
#include <string>
#include <vector>
#include <cstdint>

// Ring 4 Phase 4f: Determinism Properties Base Oracle
// Rule: Determinism means "same inputs → same execution trace"

namespace mining_test {

// ============================================================================
// DeterminismViolation - Indicates divergence between expected-identical runs
// ============================================================================

/**
 * DeterminismViolation
 *
 * Indicates that two executions which should have been
 * identical diverged.
 */
struct DeterminismViolation {
    std::string property;           // e.g. "MD1"
    std::string message;            // Human-readable divergence description
    uint64_t divergence_index{0};   // Event index where divergence occurred

    DeterminismViolation(const std::string& prop,
                        const std::string& msg,
                        uint64_t index)
        : property(prop), message(msg), divergence_index(index) {}
};

// ============================================================================
// MiningDeterminismOracle - Base class for determinism property checking
// ============================================================================

/**
 * MiningDeterminismOracle
 *
 * Compares multiple mining traces for deterministic equivalence.
 *
 * Key difference from other oracles:
 * - Takes TWO traces (reference vs candidate)
 * - Detects divergence between runs
 * - No single-trace analysis
 *
 * Determinism = same inputs → same trace.
 *
 * Design principles:
 * - Stateless across runs
 * - No global state
 * - No randomness
 * - No timing assumptions
 * - Explicit divergence reporting
 */
class MiningDeterminismOracle {
public:
    virtual ~MiningDeterminismOracle() = default;

    /**
     * Human-readable oracle name
     * e.g. "MD1: Same Seed → Identical Trace"
     */
    virtual std::string name() const = 0;

    /**
     * Compare two traces for deterministic equivalence.
     *
     * @param reference First trace (baseline)
     * @param candidate Second trace (replay)
     * @return List of determinism violations (empty if identical)
     *
     * Note: Reference and candidate should have been produced
     *       with identical inputs (seed, actions, etc.)
     */
    virtual std::vector<DeterminismViolation> check(
        const MiningTrace& reference,
        const MiningTrace& candidate
    ) const = 0;

protected:
    /**
     * Utility: compare trace lengths safely
     *
     * Two traces are length-equivalent if:
     * - Same number of events
     * - Same number of state snapshots
     */
    static bool sameLengths(const MiningTrace& a, const MiningTrace& b) {
        return a.events.size() == b.events.size() &&
               a.snapshots.size() == b.snapshots.size();
    }

    /**
     * Utility: get trace size for reporting
     */
    static size_t getEventCount(const MiningTrace& trace) {
        return trace.events.size();
    }
};

}  // namespace mining_test
