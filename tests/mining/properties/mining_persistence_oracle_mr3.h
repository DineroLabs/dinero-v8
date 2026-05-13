#pragma once

#include "mining_persistence_oracle.h"

// Ring 4 Phase 4g.2: MR3 - Partial Persistence Recovers Safely
// Property: If persistence is interrupted, recovery must result in valid state

namespace mining_test {

/**
 * MR3Oracle - Partial Persistence Recovers Safely
 *
 * Persistence property: If persistence is interrupted or partially written,
 * recovery must result in a valid, non-corrupt state.
 *
 * What MR3 detects:
 * - Crashes mid-write
 * - Torn snapshots
 * - Corrupt persistence blobs
 * - Unsafe recovery behavior
 *
 * Validation strategy:
 * 1. Inject faults (partial write, corruption) into persistence store
 * 2. Attempt recovery
 * 3. Verify recovery either:
 *    - Returns last valid snapshot, OR
 *    - Returns empty (conservative recovery)
 *    - NEVER returns corrupt/partial state
 *
 * Forbidden outcomes:
 * - Mixed old/new state
 * - Partial block records
 * - Height without block
 * - Subsidy without block
 *
 * Fault injection (mandatory):
 * - injectPartialWrite() - simulates torn write
 * - injectCorruption() - simulates corrupt data
 */
class MR3Oracle : public MiningPersistenceOracle {
public:
    std::string name() const override {
        return "MR3: Partial Persistence Recovers Safely";
    }

    std::vector<PersistenceViolation> check(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store
    ) const override;

private:
    /**
     * Validate that a recovered state is internally consistent.
     * Returns true if state is valid (no invariants broken).
     */
    bool isStateValid(const MiningState& state) const;

    /**
     * Check if recovery after fault injection returns safe state.
     * Injects fault, attempts recovery, validates outcome.
     */
    bool recoveryIsSafeAfterFault(
        DeterministicPersistenceStore& store,
        bool inject_partial_write,
        bool inject_corruption
    ) const;
};

}  // namespace mining_test
