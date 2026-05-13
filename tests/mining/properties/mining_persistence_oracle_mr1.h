#pragma once

#include "mining_persistence_oracle.h"

// Ring 4 Phase 4g.2: MR1 - State Survives Restart Correctly
// Property: Persisted state before crash must equal recovered state after restart

namespace mining_test {

/**
 * MR1Oracle - State Survives Restart Correctly
 *
 * Persistence property: If a valid state is persisted before a crash,
 * then after restart and recovery, the recovered state must be
 * equivalent to the persisted state.
 *
 * What MR1 detects:
 * - Lost blocks after restart
 * - Lost height
 * - Lost subsidy accounting
 * - Incomplete recovery
 * - Incorrect restore ordering
 *
 * Validation strategy:
 * 1. Scan trace for CRASH and RESTART action pairs
 * 2. Before each crash, persist the current state
 * 3. After each restart, recover the state
 * 4. Compare recovered vs persisted (must match)
 *
 * Allowed differences:
 * - timestamp (runtime-only)
 * - hashes_computed (transient counter)
 *
 * Forbidden differences:
 * - current_height
 * - current_tip
 * - blocks_found
 * - templates_created
 * - Any consensus-relevant state
 */
class MR1Oracle : public MiningPersistenceOracle {
public:
    std::string name() const override {
        return "MR1: State Survives Restart Correctly";
    }

    std::vector<PersistenceViolation> check(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store
    ) const override;

private:
    /**
     * Compare two states for persistence equivalence.
     * Returns true if states match for all persisted fields.
     * Ignores transient fields (timestamp, hashes_computed).
     */
    bool statesMatch(const MiningState& s1, const MiningState& s2) const;

    /**
     * Find the last state snapshot before a given action index.
     * Returns the most recent snapshot, or a default state if none exist.
     */
    MiningState findStateBeforeAction(const MiningTrace& trace, size_t action_index) const;
};

}  // namespace mining_test
