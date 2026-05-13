#include "mining_persistence_oracle_mr5.h"

// Ring 4 Phase 4g.3: MR5 Oracle Implementation

namespace mining_test {

// ============================================================================
// MR5Oracle::check - Single trace validation
// ============================================================================

std::vector<PersistenceViolation> MR5Oracle::check(
    const MiningTrace& trace,
    DeterministicPersistenceStore& store
) const {
    std::vector<PersistenceViolation> violations;

    // Single trace validation:
    // For MR5, we primarily use checkPair() to compare two traces
    // This method validates internal consistency within a single trace

    // Track persistence operations across crashes
    for (size_t i = 0; i < trace.actions.size(); i++) {
        const auto& action = trace.actions[i];

        // Persist state before crashes
        if (action.type == MiningActionType::CRASH) {
            if (!trace.snapshots.empty()) {
                size_t snapshot_idx = std::min(i, trace.snapshots.size() - 1);
                const auto& state_before_crash = trace.snapshots[snapshot_idx];
                store.persist(state_before_crash);
            }
        }

        // Verify recovered state after restarts
        if (action.type == MiningActionType::RESTART) {
            auto recovered = store.recover();

            // For single trace, we just verify recovery works
            // Determinism is checked via checkPair()
            if (!recovered.has_value() && store.snapshotVersion() > 0) {
                // Recovery failed despite having snapshot
                // This is allowed (conservative recovery), not a determinism violation
            }
        }
    }

    // For MR5, no violations in single trace mode
    // Use checkPair() for determinism validation
    return violations;
}

// ============================================================================
// MR5Oracle::checkPair - Dual trace determinism validation
// ============================================================================

std::vector<PersistenceViolation> MR5Oracle::checkPair(
    const MiningTrace& trace1,
    DeterministicPersistenceStore& store1,
    const MiningTrace& trace2,
    DeterministicPersistenceStore& store2
) const {
    std::vector<PersistenceViolation> violations;

    // Verify seeds match (prerequisite for determinism)
    if (trace1.rng_seed != trace2.rng_seed) {
        violations.push_back(violation(
            "MR5",
            "Traces have different seeds - cannot validate determinism",
            0
        ));
        return violations;
    }

    // Verify action sequences match
    if (trace1.actions.size() != trace2.actions.size()) {
        violations.push_back(violation(
            "MR5",
            "Traces have different action counts",
            0
        ));
        return violations;
    }

    // Track recovered states from both traces
    std::vector<MiningState> recovered_states1;
    std::vector<MiningState> recovered_states2;

    // Process both traces in parallel
    for (size_t i = 0; i < trace1.actions.size(); i++) {
        const auto& action1 = trace1.actions[i];
        const auto& action2 = trace2.actions[i];

        // Verify actions match
        if (action1.type != action2.type) {
            violations.push_back(violation(
                "MR5",
                "Action types diverged at index " + std::to_string(i),
                i
            ));
            return violations;
        }

        // Persist state before crashes
        if (action1.type == MiningActionType::CRASH) {
            if (!trace1.snapshots.empty() && !trace2.snapshots.empty()) {
                size_t snap_idx1 = std::min(i, trace1.snapshots.size() - 1);
                size_t snap_idx2 = std::min(i, trace2.snapshots.size() - 1);

                store1.persist(trace1.snapshots[snap_idx1]);
                store2.persist(trace2.snapshots[snap_idx2]);
            }
        }

        // Recover and compare states after restarts
        if (action1.type == MiningActionType::RESTART) {
            auto recovered1 = store1.recover();
            auto recovered2 = store2.recover();

            // Both must have same recovery outcome
            if (recovered1.has_value() != recovered2.has_value()) {
                violations.push_back(violation(
                    "MR5",
                    "Recovery diverged: one succeeded, one failed",
                    i
                ));
                return violations;
            }

            // If both recovered, states must match
            if (recovered1.has_value() && recovered2.has_value()) {
                if (!recoveredStatesMatch(*recovered1, *recovered2)) {
                    violations.push_back(violation(
                        "MR5",
                        "Recovered states diverged after restart",
                        i
                    ));
                    return violations;
                }

                recovered_states1.push_back(*recovered1);
                recovered_states2.push_back(*recovered2);
            }
        }
    }

    // If we got here, determinism is preserved
    return violations;
}

// ============================================================================
// MR5Oracle::recoveredStatesMatch - Compare recovered states
// ============================================================================

bool MR5Oracle::recoveredStatesMatch(
    const MiningState& state1,
    const MiningState& state2
) const {
    // For determinism, ALL fields must match
    // (Unlike MR1 which ignores some transient fields)

    // This is strict equality check for determinism
    return state1 == state2;
}

}  // namespace mining_test
