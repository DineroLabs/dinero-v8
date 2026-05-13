#include "mining_persistence_oracle_mr3.h"

// Ring 4 Phase 4g.2: MR3 Oracle Implementation

namespace mining_test {

// ============================================================================
// MR3Oracle::check - Main property checking logic
// ============================================================================

std::vector<PersistenceViolation> MR3Oracle::check(
    const MiningTrace& trace,
    DeterministicPersistenceStore& store
) const {
    std::vector<PersistenceViolation> violations;

    // Scan trace for CRASH events to trigger fault injection tests
    for (size_t i = 0; i < trace.actions.size(); i++) {
        const auto& action = trace.actions[i];

        // When we find a CRASH, test fault injection scenarios
        if (action.type == MiningActionType::CRASH) {
            // Find the state just before crash
            if (!trace.snapshots.empty()) {
                size_t snapshot_idx = std::min(i, trace.snapshots.size() - 1);
                const auto& state_before_crash = trace.snapshots[snapshot_idx];

                // Persist the state
                store.persist(state_before_crash);

                // Test 1: Partial write recovery
                if (!recoveryIsSafeAfterFault(store, true, false)) {
                    violations.push_back(violation(
                        "MR3",
                        "Partial write recovery returned invalid state",
                        i
                    ));
                }

                // Re-persist for next test
                store.persist(state_before_crash);

                // Test 2: Corruption recovery
                if (!recoveryIsSafeAfterFault(store, false, true)) {
                    violations.push_back(violation(
                        "MR3",
                        "Corruption recovery returned invalid state",
                        i
                    ));
                }

                // Re-persist for safety
                store.persist(state_before_crash);
            }
        }
    }

    // If no crashes in trace, test fault injection once with default state
    bool has_crash = false;
    for (const auto& action : trace.actions) {
        if (action.type == MiningActionType::CRASH) {
            has_crash = true;
            break;
        }
    }

    if (!has_crash && !trace.snapshots.empty()) {
        // Persist final state
        store.persist(trace.snapshots.back());

        // Test partial write recovery
        if (!recoveryIsSafeAfterFault(store, true, false)) {
            violations.push_back(violation(
                "MR3",
                "Partial write recovery failed on final state",
                trace.actions.size()
            ));
        }

        // Re-persist
        store.persist(trace.snapshots.back());

        // Test corruption recovery
        if (!recoveryIsSafeAfterFault(store, false, true)) {
            violations.push_back(violation(
                "MR3",
                "Corruption recovery failed on final state",
                trace.actions.size()
            ));
        }
    }

    return violations;
}

// ============================================================================
// MR3Oracle::isStateValid - Validate state consistency
// ============================================================================

bool MR3Oracle::isStateValid(const MiningState& state) const {
    // Check invariants that must hold in any valid state

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

    // All invariants passed
    return true;
}

// ============================================================================
// MR3Oracle::recoveryIsSafeAfterFault - Test fault injection
// ============================================================================

bool MR3Oracle::recoveryIsSafeAfterFault(
    DeterministicPersistenceStore& store,
    bool inject_partial_write,
    bool inject_corruption
) const {
    // Inject the fault
    if (inject_partial_write) {
        store.injectPartialWrite();
    }
    if (inject_corruption) {
        store.injectCorruption();
    }

    // Attempt recovery
    auto recovered = store.recover();

    // Recovery is safe if:
    // 1. Recovery failed (conservative - this is ALLOWED)
    if (!recovered.has_value()) {
        return true;  // Safe: conservative recovery
    }

    // 2. Recovery succeeded with valid state
    if (isStateValid(*recovered)) {
        return true;  // Safe: valid state recovered
    }

    // 3. If we got here, recovery returned an INVALID state
    // This is FORBIDDEN by MR3
    return false;
}

}  // namespace mining_test
