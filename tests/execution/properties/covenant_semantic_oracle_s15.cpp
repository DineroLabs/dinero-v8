#include "covenant_semantic_oracle_s15.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CovenantViolation> S15Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CovenantViolation> violations;

    // Only check if state transitions present
    if (!hasStateTransitions(trace)) {
        return violations;  // No state transitions, property trivially holds
    }

    // Check state transition validity
    auto validation_error = verifyTransitionsValidated(trace);
    if (validation_error) {
        CovenantViolation violation(
            getName(),
            "State transition violated: " + *validation_error,
            0
        );
        violation.details = "State machine transition violated covenant rules";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S15Oracle::verifyTransitionsValidated(const ExecutionTrace& trace) const {
    // Check 1: Introspection validates state
    auto introspection_ops = getIntrospectionOps(trace);

    if (introspection_ops.empty()) {
        return "State transitions present but no validation introspection";
    }

    // Check 2: Transition order is valid
    auto order_error = verifyTransitionOrder(trace);
    if (order_error) {
        return order_error;
    }

    // Check 3: No invalid states reached
    auto invalid_state_error = verifyNoInvalidState(trace);
    if (invalid_state_error) {
        return invalid_state_error;
    }

    return std::nullopt;  // Transitions validated
}

std::optional<std::string> S15Oracle::verifyTransitionOrder(const ExecutionTrace& trace) const {
    // Verify state transitions occur in valid order
    // For Phase 7d: Check that introspection ops are sequential

    auto introspection_ops = getIntrospectionOps(trace);
    if (introspection_ops.size() < 2) {
        return std::nullopt;  // Single or no transition
    }

    // Verify sequential ordering
    for (size_t i = 1; i < introspection_ops.size(); i++) {
        if (introspection_ops[i].step <= introspection_ops[i-1].step) {
            return "State transitions not in sequential order";
        }

        // Verify each transition builds on previous
        // (step numbers should increase monotonically)
        if (introspection_ops[i].step - introspection_ops[i-1].step > 1000) {
            return "Large gap between state transitions (potential skip)";
        }
    }

    return std::nullopt;  // Transition order valid
}

std::optional<std::string> S15Oracle::verifyNoInvalidState(const ExecutionTrace& trace) const {
    // Verify execution doesn't reach invalid state
    // For Phase 7d: Check that all state-validating operations succeed

    auto introspection_ops = getIntrospectionOps(trace);

    // If execution succeeds, all state validations should have passed
    if (wasSuccessful(trace)) {
        for (const auto& op : introspection_ops) {
            if (!op.success) {
                return "Execution succeeded despite failed state validation at step " +
                       std::to_string(op.step);
            }
        }
    }

    // If execution fails, at least one validation should have failed
    // (indicating invalid state transition was rejected)
    if (!wasSuccessful(trace)) {
        bool any_failed = false;
        for (const auto& op : introspection_ops) {
            if (!op.success) {
                any_failed = true;
                break;
            }
        }

        if (!any_failed && !introspection_ops.empty()) {
            return "Execution failed but all state validations passed (inconsistency)";
        }
    }

    return std::nullopt;  // No invalid state
}

} // namespace test
} // namespace execution
} // namespace dinero
