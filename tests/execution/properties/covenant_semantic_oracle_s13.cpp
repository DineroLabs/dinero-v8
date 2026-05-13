#include "covenant_semantic_oracle_s13.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CovenantViolation> S13Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CovenantViolation> violations;

    // Only check if multiple covenant operations present
    size_t covenant_count = countCovenantOps(trace);
    if (covenant_count < 2) {
        return violations;  // Single or no covenant, property trivially holds
    }

    // Check covenant composition safety
    auto interference_error = verifyNoInterference(trace);
    if (interference_error) {
        CovenantViolation violation(
            getName(),
            "Composition safety violated: " + *interference_error,
            0
        );
        violation.details = "Multiple covenants interfere with each other";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S13Oracle::verifyNoInterference(const ExecutionTrace& trace) const {
    // Check 1: All covenant constraints checked
    auto checked_error = verifyAllChecked(trace);
    if (checked_error) {
        return checked_error;
    }

    // Check 2: No bypass through composition
    auto bypass_error = verifyNoBypass(trace);
    if (bypass_error) {
        return bypass_error;
    }

    // Check 3: Operations execute in order
    auto introspection_ops = getIntrospectionOps(trace);
    if (introspection_ops.size() < 2) {
        return std::nullopt;  // Not enough ops to interfere
    }

    // Verify operations are sequentially ordered
    for (size_t i = 1; i < introspection_ops.size(); i++) {
        if (introspection_ops[i].step <= introspection_ops[i-1].step) {
            return "Covenant operations not sequentially ordered";
        }
    }

    return std::nullopt;  // No interference
}

std::optional<std::string> S13Oracle::verifyAllChecked(const ExecutionTrace& trace) const {
    // Verify each covenant operation was checked
    auto introspection_ops = getIntrospectionOps(trace);

    size_t successful_checks = 0;
    for (const auto& op : introspection_ops) {
        if (op.success) {
            successful_checks++;
        }
    }

    // At least one check must succeed if execution succeeds
    if (wasSuccessful(trace) && successful_checks == 0) {
        return "Execution succeeded but no covenant checks succeeded";
    }

    return std::nullopt;  // All checked
}

std::optional<std::string> S13Oracle::verifyNoBypass(const ExecutionTrace& trace) const {
    // Verify composition doesn't bypass individual constraints
    // For Phase 7d: Check that all introspection ops complete

    auto introspection_ops = getIntrospectionOps(trace);
    size_t total_ops = introspection_ops.size();
    size_t completed_ops = 0;

    for (const auto& op : introspection_ops) {
        if (op.success) {
            completed_ops++;
        }
    }

    // If execution succeeds, all covenant ops should have completed
    if (wasSuccessful(trace) && completed_ops < total_ops) {
        return "Some covenant checks bypassed in composition (completed=" +
               std::to_string(completed_ops) + ", total=" +
               std::to_string(total_ops) + ")";
    }

    return std::nullopt;  // No bypass
}

} // namespace test
} // namespace execution
} // namespace dinero
