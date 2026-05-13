#pragma once

#include "mining_persistence_oracle.h"

// Ring 4 Phase 4g.3: MR4 - Restart Converges to a Valid State
// Property: After crashes/faults/restarts, system eventually recovers to valid state

namespace mining_test {

/**
 * MR4Oracle - Restart Converges to a Valid State
 *
 * Persistence property: After any sequence of crashes, partial persists,
 * corrupt persists, and restarts, the system must eventually recover
 * into a valid state.
 *
 * This is a convergence guarantee (eventual safety), not immediate correctness.
 *
 * What MR4 detects:
 * - Recovery deadlocks
 * - Oscillating invalid states
 * - Persistent corruption loops
 * - Recovery that never stabilizes
 * - "Zombie" partially-recovered states
 *
 * Validation strategy:
 * 1. Identify restart sequences in trace (CRASH → RESTART)
 * 2. Allow multiple recovery attempts
 * 3. Observe the final recovered state
 * 4. Verify final state is valid
 *
 * Valid state requirements:
 * - Valid chain height
 * - No duplicate blocks
 * - No partial blocks
 * - Subsidy invariants hold
 * - State is internally consistent
 *
 * Does NOT require:
 * - Same state as before crash
 * - Maximal recovery
 *
 * Does require:
 * - A safe, coherent state
 * - Forward progress possible
 */
class MR4Oracle : public MiningPersistenceOracle {
public:
    std::string name() const override {
        return "MR4: Restart Converges to a Valid State";
    }

    std::vector<PersistenceViolation> check(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store
    ) const override;

private:
    /**
     * Validate that a recovered state is internally consistent.
     * Returns true if state is valid (satisfies all invariants).
     */
    bool isStateValid(const MiningState& state) const;

    /**
     * Check if a restart sequence converges to a valid state.
     * Tracks crash → restart pairs and validates final recovery.
     */
    bool restartSequenceConverges(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store,
        size_t restart_index
    ) const;
};

}  // namespace mining_test
