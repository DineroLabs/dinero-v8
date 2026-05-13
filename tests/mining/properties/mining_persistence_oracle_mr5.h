#pragma once

#include "mining_persistence_oracle.h"

// Ring 4 Phase 4g.3: MR5 - Persistence Does Not Break Determinism
// Property: Persistence and recovery must preserve Phase 4f determinism guarantees

namespace mining_test {

/**
 * MR5Oracle - Persistence Does Not Break Determinism
 *
 * Persistence property: Persistence and recovery must preserve
 * Phase 4f determinism guarantees (MD1-MD5).
 *
 * This ties Phase 4g back to Phase 4f.
 *
 * What MR5 detects:
 * - Persistence introducing entropy
 * - Recovery path non-determinism
 * - Checkpoint ordering bugs
 * - Replay divergence after restart
 * - State-dependent randomness
 *
 * Validation strategy:
 * 1. Run two identical traces with:
 *    - Same seed
 *    - Same actions
 *    - Same persistence faults
 * 2. Perform crash + persist + recover
 * 3. Compare recovered traces
 * 4. Ensure RecoveredTrace_A == RecoveredTrace_B
 *
 * Validation rule:
 * Event-by-event equality, exactly like MD1.
 *
 * Forbidden outcomes:
 * - Same seed + same faults → different recovered traces
 * - Recovery path differs
 * - Persisted state differs
 * - Determinism breaks post-restart
 *
 * Allowed outcomes:
 * - Different seeds → divergence (expected)
 * - Different fault injections → divergence (expected)
 *
 * Relationship to Phase 4f:
 * - MD1: Same seed → same trace
 * - MD2: Restart replay deterministic
 * - MD5: Crash recovery deterministic
 * - MR5: Persistence preserves MD1-MD5
 */
class MR5Oracle : public MiningPersistenceOracle {
public:
    std::string name() const override {
        return "MR5: Persistence Does Not Break Determinism";
    }

    std::vector<PersistenceViolation> check(
        const MiningTrace& trace,
        DeterministicPersistenceStore& store
    ) const override;

    /**
     * Check determinism across two traces with same seed.
     * This is the primary MR5 validation method.
     */
    std::vector<PersistenceViolation> checkPair(
        const MiningTrace& trace1,
        DeterministicPersistenceStore& store1,
        const MiningTrace& trace2,
        DeterministicPersistenceStore& store2
    ) const;

private:
    /**
     * Compare two recovered states for determinism.
     * Returns true if states are identical.
     */
    bool recoveredStatesMatch(
        const MiningState& state1,
        const MiningState& state2
    ) const;
};

}  // namespace mining_test
