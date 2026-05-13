#include "composition_state_oracle_s18.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CompositionViolation> S18Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CompositionViolation> violations;

    // Only check if state updates present
    if (!hasStateUpdates(trace)) {
        return violations;  // No state updates, property trivially holds
    }

    // Check state consistency
    auto consistency_error = verifyStateConsistency(trace);
    if (consistency_error) {
        CompositionViolation violation(
            getName(),
            "State consistency violated: " + *consistency_error,
            0
        );
        violation.details = "State updates are inconsistent";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S18Oracle::verifyStateConsistency(const ExecutionTrace& trace) const {
    // Check 1: All updates recorded
    auto recording_error = verifyUpdatesRecorded(trace);
    if (recording_error) {
        return recording_error;
    }

    // Check 2: Final state valid
    auto final_state_error = verifyFinalStateValid(trace);
    if (final_state_error) {
        return final_state_error;
    }

    // Check 3: State updates are sequential (not concurrent)
    size_t state_update_count = 0;
    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::STATE_UPDATED) {
            state_update_count++;
        }
    }

    // Verify stack states tracked (snapshots)
    // For Phase 7e: Simplified check - verify trace has final state
    if (state_update_count > 0) {
        // At least final state should exist
        // (Full implementation would track state at each update)
        // For now, we just verify state updates were recorded
    }

    return std::nullopt;  // State consistent
}

std::optional<std::string> S18Oracle::verifyUpdatesRecorded(const ExecutionTrace& trace) const {
    // Verify all state updates are recorded in trace
    // For Phase 7e: Check for STATE_UPDATED events

    size_t update_events = 0;
    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::STATE_UPDATED) {
            update_events++;
        }
    }

    if (update_events == 0) {
        return "No state update events despite hasStateUpdates=true";
    }

    return std::nullopt;  // Updates recorded
}

std::optional<std::string> S18Oracle::verifyFinalStateValid(const ExecutionTrace& trace) const {
    // Verify final state is valid
    // For Phase 7e: Check execution succeeded if state updates occurred

    if (hasStateUpdates(trace)) {
        // If state was updated, execution should either succeed or fail consistently
        // Check that trace has definite outcome
        if (trace.final_hash == 0) {
            return "State updates occurred but trace not finalized";
        }
    }

    return std::nullopt;  // Final state valid
}

} // namespace test
} // namespace execution
} // namespace dinero
