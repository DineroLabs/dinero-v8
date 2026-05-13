#pragma once

#include "mining_persistence_oracle.h"
#include <set>

// Ring 4 Phase 4g.2: MR2 - No State Duplication After Crash
// Property: After crash and recovery, no state element may appear more than once

namespace mining_test {

/**
 * MR2Oracle - No State Duplication After Crash
 *
 * Persistence property: After crash and recovery, no state element
 * may appear more than once.
 *
 * What MR2 detects:
 * - Replay of persisted blocks
 * - Duplicate height entries
 * - Duplicate subsidy application
 * - Re-application of persisted work
 *
 * Validation strategy:
 * 1. Track all block IDs and heights seen across trace
 * 2. After each RESTART, verify no duplicates
 * 3. Ensure subsidy totals are monotonic (no replay)
 *
 * Violation conditions:
 * - Any duplicate block IDs
 * - Any duplicate heights
 * - Subsidy increased without new mining events
 */
class MR2Oracle : public MiningPersistenceOracle {
public:
    std::string name() const override {
        return "MR2: No State Duplication After Crash";
    }

    std::vector<PersistenceViolation> check(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store
    ) const override;

private:
    /**
     * Check for duplicate block IDs in the trace after a restart point.
     */
    bool hasDuplicateBlocks(
        const MiningTrace& trace,
        size_t restart_index
    ) const;

    /**
     * Check for duplicate heights in the trace after a restart point.
     */
    bool hasDuplicateHeights(
        const MiningTrace& trace,
        size_t restart_index
    ) const;

    /**
     * Check if subsidy increased without new mining events.
     */
    bool hasSubsidyDuplication(
        const MiningTrace& trace,
        size_t restart_index
    ) const;
};

}  // namespace mining_test
