#include "composition_state_oracle_s16.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CompositionViolation> S16Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CompositionViolation> violations;

    // Only check if multiple inputs present
    if (!hasMultipleInputs(trace)) {
        return violations;  // Single input, property trivially holds
    }

    // Check input isolation
    auto isolation_error = verifyInputsIsolated(trace);
    if (isolation_error) {
        CompositionViolation violation(
            getName(),
            "Input isolation violated: " + *isolation_error,
            0
        );
        violation.details = "Inputs interfere with each other";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S16Oracle::verifyInputsIsolated(const ExecutionTrace& trace) const {
    // Check 1: Isolation markers present
    if (!areOperationsIsolated(trace)) {
        return "Multi-input execution without isolation markers";
    }

    // Check 2: No cross-input operations
    auto cross_ops_error = verifyNoCrossInputOps(trace);
    if (cross_ops_error) {
        return cross_ops_error;
    }

    // Check 3: Independent execution
    auto independence_error = verifyIndependentExecution(trace);
    if (independence_error) {
        return independence_error;
    }

    return std::nullopt;  // Inputs isolated
}

std::optional<std::string> S16Oracle::verifyNoCrossInputOps(const ExecutionTrace& trace) const {
    // Verify no operations cross input boundaries
    // For Phase 7e: Check that each input has isolated events

    size_t multi_input_count = 0;
    size_t isolated_count = 0;

    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::MULTI_INPUT_START) {
            multi_input_count++;
        }
        if (event.type == ExecutionEventType::INPUT_ISOLATED) {
            isolated_count++;
        }
    }

    // Each multi-input should have corresponding isolation marker
    if (multi_input_count > 0 && isolated_count < multi_input_count) {
        return "Not all inputs marked as isolated (multi=" +
               std::to_string(multi_input_count) + ", isolated=" +
               std::to_string(isolated_count) + ")";
    }

    return std::nullopt;  // No cross-input operations
}

std::optional<std::string> S16Oracle::verifyIndependentExecution(const ExecutionTrace& trace) const {
    // Verify inputs execute independently
    // For Phase 7e: Check that operations don't share state

    // If execution succeeds with multiple inputs, check for combined success marker
    if (hasMultipleInputs(trace) && wasSuccessful(trace)) {
        bool has_combined_success = false;

        for (const auto& event : trace.events) {
            if (event.type == ExecutionEventType::COMBINED_SUCCESS) {
                has_combined_success = true;
                break;
            }
        }

        // Multi-input success should have combined success marker
        if (!has_combined_success) {
            return "Multi-input execution succeeded without combined success marker";
        }
    }

    return std::nullopt;  // Independent execution
}

} // namespace test
} // namespace execution
} // namespace dinero
