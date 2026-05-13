#include "covenant_semantic_oracle_s11.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CovenantViolation> S11Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CovenantViolation> violations;

    // Only check if covenant operations present
    if (!hasCovenantOps(trace)) {
        return violations;  // No covenant, property trivially holds
    }

    // Check constraint enforcement
    auto enforcement_error = verifyConstraintsEnforced(trace);
    if (enforcement_error) {
        CovenantViolation violation(
            getName(),
            "Constraint enforcement violated: " + *enforcement_error,
            0
        );
        violation.details = "Covenant constraints not properly enforced";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S11Oracle::verifyConstraintsEnforced(const ExecutionTrace& trace) const {
    // Check 1: Introspection operations present
    auto introspection_error = verifyIntrospectionPresent(trace);
    if (introspection_error) {
        return introspection_error;
    }

    // Check 2: Validation not bypassed
    auto bypass_error = verifyNotBypassed(trace);
    if (bypass_error) {
        return bypass_error;
    }

    // Check 3: Success only if constraints satisfied
    if (hasCovenantOps(trace) && !wasSuccessful(trace)) {
        // Covenant present but failed - constraints enforced correctly
        return std::nullopt;
    }

    if (hasCovenantOps(trace) && wasSuccessful(trace)) {
        // Covenant present and succeeded - verify introspection ran
        size_t introspection_count = getIntrospectionOps(trace).size();
        if (introspection_count == 0) {
            return "Covenant succeeded without introspection operations";
        }
    }

    return std::nullopt;  // Constraints enforced
}

std::optional<std::string> S11Oracle::verifyIntrospectionPresent(const ExecutionTrace& trace) const {
    // If covenant ops present, introspection should occur
    if (hasCovenantOps(trace)) {
        auto introspection_ops = getIntrospectionOps(trace);
        if (introspection_ops.empty()) {
            return "Covenant operations present but no introspection";
        }
    }

    return std::nullopt;  // Introspection present
}

std::optional<std::string> S11Oracle::verifyNotBypassed(const ExecutionTrace& trace) const {
    // Check for bypass patterns:
    // 1. Covenant ops present but all skipped
    // 2. Introspection ops present but all failed

    size_t covenant_count = countCovenantOps(trace);
    if (covenant_count == 0) {
        return std::nullopt;  // No covenant to bypass
    }

    // Check if all introspection operations failed (potential bypass)
    auto introspection_ops = getIntrospectionOps(trace);
    bool all_failed = true;

    for (const auto& op : introspection_ops) {
        if (op.success) {
            all_failed = false;
            break;
        }
    }

    if (all_failed && !introspection_ops.empty()) {
        return "All introspection operations failed (potential bypass)";
    }

    return std::nullopt;  // Not bypassed
}

} // namespace test
} // namespace execution
} // namespace dinero
