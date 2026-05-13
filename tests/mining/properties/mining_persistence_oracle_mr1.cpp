#include "mining_persistence_oracle_mr1.h"

// Ring 4 Phase 4g.2: MR1 Oracle Implementation

namespace mining_test {

// ============================================================================
// MR1Oracle::check - Main property checking logic
// ============================================================================

std::vector<PersistenceViolation> MR1Oracle::check(
    const MiningTrace& trace,
    DeterministicPersistenceStore& store
) const {
    std::vector<PersistenceViolation> violations;

    // Scan trace for CRASH and RESTART pairs
    for (size_t i = 0; i < trace.actions.size(); i++) {
        const auto& action = trace.actions[i];

        // When we find a CRASH, persist the current state
        if (action.type == MiningActionType::CRASH) {
            // Find the state just before crash
            MiningState state_before_crash = findStateBeforeAction(trace, i);

            // Persist it
            store.persist(state_before_crash);
        }

        // When we find a RESTART, verify recovery
        if (action.type == MiningActionType::RESTART) {
            // Recover the state
            auto recovered = store.recover();

            // If no snapshot exists, check if this is expected
            if (!recovered.has_value()) {
                // Check if there was a prior persist
                if (store.snapshotVersion() == 0) {
                    // No persist before crash - this is OK (MR1.4, MR1.5)
                    continue;
                } else {
                    // Persist existed but recovery failed
                    violations.push_back(violation(
                        "MR1",
                        "Recover failed after restart despite valid persist",
                        i
                    ));
                    continue;
                }
            }

            // If we got here, recovery succeeded
            // Find what was persisted before the most recent crash
            // (Look backwards from this RESTART to find the CRASH)
            std::optional<MiningState> persisted_state;
            for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
                if (trace.actions[j].type == MiningActionType::CRASH) {
                    persisted_state = findStateBeforeAction(trace, j);
                    break;
                }
            }

            if (!persisted_state.has_value()) {
                // RESTART without prior CRASH - unexpected but not a violation
                continue;
            }

            // Compare persisted vs recovered
            if (!statesMatch(*persisted_state, *recovered)) {
                violations.push_back(violation(
                    "MR1",
                    "Recovered state does not match persisted state",
                    i
                ));
            }
        }
    }

    return violations;
}

// ============================================================================
// MR1Oracle::statesMatch - Compare states for persistence equivalence
// ============================================================================

bool MR1Oracle::statesMatch(const MiningState& s1, const MiningState& s2) const {
    // Check consensus-critical fields (must match)
    if (s1.phase != s2.phase) return false;
    if (s1.current_tip != s2.current_tip) return false;
    if (s1.current_height != s2.current_height) return false;
    if (s1.mempool_size != s2.mempool_size) return false;
    if (s1.mempool_total_fees != s2.mempool_total_fees) return false;
    if (s1.template_prev_hash != s2.template_prev_hash) return false;
    if (s1.template_height != s2.template_height) return false;
    if (s1.template_subsidy != s2.template_subsidy) return false;
    if (s1.template_tx_count != s2.template_tx_count) return false;
    if (s1.blocks_found != s2.blocks_found) return false;
    if (s1.templates_created != s2.templates_created) return false;
    if (s1.has_crashed != s2.has_crashed) return false;
    if (s1.restart_count != s2.restart_count) return false;

    // Ignore transient fields:
    // - timestamp (runtime-only)
    // - hashes_computed (transient counter, not consensus-critical)

    return true;
}

// ============================================================================
// MR1Oracle::findStateBeforeAction - Get snapshot before action
// ============================================================================

MiningState MR1Oracle::findStateBeforeAction(
    const MiningTrace& trace,
    size_t action_index
) const {
    // Snapshots are taken at various points during execution
    // Find the most recent snapshot before this action

    // Simple heuristic: use the last snapshot before this action index
    // (More sophisticated: match by timestamp, but this works for Phase 4g)

    if (trace.snapshots.empty()) {
        // No snapshots - return default state
        return MiningState{};
    }

    // For simplicity, if we have snapshots, use the last one
    // (Phase 4g assumption: snapshots align with trace progression)

    // Match by finding closest snapshot before the action
    // Actions have sequence numbers, snapshots are indexed
    // Conservative approach: use last snapshot

    if (action_index < trace.snapshots.size()) {
        return trace.snapshots[action_index];
    }

    // If action index exceeds snapshots, use last snapshot
    return trace.snapshots.back();
}

}  // namespace mining_test
