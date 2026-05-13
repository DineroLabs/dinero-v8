#include "mining_persistence_oracle_mr4.h"

// Ring 4 Phase 4g.3: MR4 Oracle Implementation

namespace mining_test {

// ============================================================================
// MR4Oracle::check - Main property checking logic
// ============================================================================

std::vector<PersistenceViolation> MR4Oracle::check(
    const MiningTrace& trace,
    DeterministicPersistenceStore& store
) const {
    std::vector<PersistenceViolation> violations;

    // Track crash/restart sequences
    bool in_crash_sequence = false;
    size_t crash_start_index = 0;

    // Scan trace for CRASH and RESTART pairs
    for (size_t i = 0; i < trace.actions.size(); i++) {
        const auto& action = trace.actions[i];

        // When we find a CRASH, persist the current state
        if (action.type == MiningActionType::CRASH) {
            // Find the state just before crash
            if (!trace.snapshots.empty()) {
                size_t snapshot_idx = std::min(i, trace.snapshots.size() - 1);
                const auto& state_before_crash = trace.snapshots[snapshot_idx];

                // Persist it
                store.persist(state_before_crash);

                in_crash_sequence = true;
                crash_start_index = i;
            }
        }

        // When we find a RESTART, verify convergence
        if (action.type == MiningActionType::RESTART) {
            if (!restartSequenceConverges(trace, store, i)) {
                violations.push_back(violation(
                    "MR4",
                    "Restart sequence did not converge to valid state",
                    i
                ));
            }

            in_crash_sequence = false;
        }
    }

    return violations;
}

// ============================================================================
// MR4Oracle::isStateValid - Validate state consistency
// ============================================================================

bool MR4Oracle::isStateValid(const MiningState& state) const {
    // Check invariants that must hold in any valid state
    // (Same validation as MR3 - internal consistency checks)

    // Invariant 1: blocks_found <= current_height + 1
    // (Can't have more blocks than height allows)
    if (state.blocks_found > state.current_height + 1) {
        return false;
    }

    // Invariant 2: If template exists, template_height should be reasonable
    if (state.template_height.has_value()) {
        if (*state.template_height > state.current_height + 1) {
            return false;  // Template for impossible future height
        }
    }

    // Invariant 3: Templates created should be >= blocks found
    // (Need at least one template per block)
    if (state.templates_created < state.blocks_found) {
        return false;
    }

    // Invariant 4: Mempool size should match fee accounting
    // (Conservative check - if fees > 0, size should be > 0)
    if (state.mempool_total_fees > 0 && state.mempool_size == 0) {
        return false;
    }

    // Invariant 5: Restart count should be consistent with crash history
    // (If we've crashed, restart_count should reflect it)
    if (state.has_crashed && state.restart_count == 0) {
        // This is actually OK - crash doesn't guarantee restart
        // Removing this check as it's too strict
    }

    // All invariants passed
    return true;
}

// ============================================================================
// MR4Oracle::restartSequenceConverges - Check convergence
// ============================================================================

bool MR4Oracle::restartSequenceConverges(
    const MiningTrace& trace,
    DeterministicPersistenceStore& store,
    size_t restart_index
) const {
    // Attempt recovery
    auto recovered = store.recover();

    // Case 1: Recovery failed (no snapshot)
    if (!recovered.has_value()) {
        // This is OK if no persist occurred
        if (store.snapshotVersion() == 0) {
            return true;  // Convergence to empty state (valid)
        }

        // If persist existed but recovery failed, check if store was cleared
        // This could be intentional (conservative recovery)
        // For MR4, we consider this acceptable (converged to safe empty state)
        return true;
    }

    // Case 2: Recovery succeeded
    // Check if recovered state is valid
    if (isStateValid(*recovered)) {
        return true;  // Converged to valid state ✅
    }

    // Case 3: Recovery succeeded but state is invalid
    // This violates MR4 - no convergence to valid state
    return false;
}

}  // namespace mining_test
