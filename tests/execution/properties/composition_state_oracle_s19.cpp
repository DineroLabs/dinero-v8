#include "composition_state_oracle_s19.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CompositionViolation> S19Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CompositionViolation> violations;

    // Only check if multiple inputs present
    if (!hasMultipleInputs(trace)) {
        return violations;  // Single input, property trivially holds
    }

    // Check cross-input invariants
    auto invariant_error = verifyCrossInputInvariants(trace);
    if (invariant_error) {
        CompositionViolation violation(
            getName(),
            "Cross-input invariants violated: " + *invariant_error,
            0
        );
        violation.details = "Invariants don't hold across inputs";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S19Oracle::verifyCrossInputInvariants(const ExecutionTrace& trace) const {
    // Check 1: Per-input invariants
    auto per_input_error = verifyPerInputInvariants(trace);
    if (per_input_error) {
        return per_input_error;
    }

    // Check 2: Global invariants
    auto global_error = verifyGlobalInvariants(trace);
    if (global_error) {
        return global_error;
    }

    // Check 3: Input count consistency
    size_t input_count = getInputCount(trace);
    if (input_count > 1) {
        // Verify each input is properly isolated and checked
        size_t isolated_count = 0;
        for (const auto& event : trace.events) {
            if (event.type == ExecutionEventType::INPUT_ISOLATED) {
                isolated_count++;
            }
        }

        if (isolated_count < input_count) {
            return "Not all inputs have isolation markers (inputs=" +
                   std::to_string(input_count) + ", isolated=" +
                   std::to_string(isolated_count) + ")";
        }
    }

    return std::nullopt;  // Invariants hold
}

std::optional<std::string> S19Oracle::verifyPerInputInvariants(const ExecutionTrace& trace) const {
    // Verify invariants for each input
    // For Phase 7e: Check that each input has success/fail markers

    size_t input_count = getInputCount(trace);
    if (input_count == 0) {
        return "Zero inputs detected";
    }

    // Each input should have either success or failure outcome
    // Verified by checking for execution events
    size_t script_starts = 0;
    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::SCRIPT_START) {
            script_starts++;
        }
    }

    // At least one script start per input
    if (script_starts < input_count) {
        return "Not enough script start events for input count";
    }

    return std::nullopt;  // Per-input invariants hold
}

std::optional<std::string> S19Oracle::verifyGlobalInvariants(const ExecutionTrace& trace) const {
    // Verify global invariants across all inputs
    // For Phase 7e: Check that trace is finalized and deterministic

    if (!isDeterministic(trace)) {
        return "Global invariant violated: trace is non-deterministic";
    }

    // If multi-input succeeded, should have combined success marker
    if (hasMultipleInputs(trace) && wasSuccessful(trace)) {
        bool has_combined = false;
        for (const auto& event : trace.events) {
            if (event.type == ExecutionEventType::COMBINED_SUCCESS) {
                has_combined = true;
                break;
            }
        }

        if (!has_combined) {
            return "Multi-input success without combined success marker";
        }
    }

    return std::nullopt;  // Global invariants hold
}

} // namespace test
} // namespace execution
} // namespace dinero
