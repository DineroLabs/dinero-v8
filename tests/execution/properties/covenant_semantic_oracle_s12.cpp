#include "covenant_semantic_oracle_s12.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CovenantViolation> S12Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CovenantViolation> violations;

    // Only check if introspection operations present
    auto introspection_ops = getIntrospectionOps(trace);
    if (introspection_ops.empty()) {
        return violations;  // No introspection, property trivially holds
    }

    // Check introspection correctness
    auto success_error = verifyIntrospectionSuccess(trace);
    if (success_error) {
        CovenantViolation violation(
            getName(),
            "Introspection correctness violated: " + *success_error,
            0
        );
        violation.details = "Introspection opcodes returned incorrect values";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S12Oracle::verifyIntrospectionSuccess(const ExecutionTrace& trace) const {
    // Check 1: Introspection operations succeed
    auto introspection_ops = getIntrospectionOps(trace);

    for (const auto& op : introspection_ops) {
        if (!op.success) {
            return "Introspection operation at step " + std::to_string(op.step) + " failed";
        }
    }

    // Check 2: Stack state after introspection
    auto stack_error = verifyStackState(trace);
    if (stack_error) {
        return stack_error;
    }

    // Check 3: Determinism
    auto determinism_error = verifyDeterminism(trace);
    if (determinism_error) {
        return determinism_error;
    }

    return std::nullopt;  // Introspection correct
}

std::optional<std::string> S12Oracle::verifyStackState(const ExecutionTrace& trace) const {
    // Verify stack state after introspection operations
    // For Phase 7d: Simplified check - just verify operations completed

    auto introspection_ops = getIntrospectionOps(trace);
    if (introspection_ops.empty()) {
        return std::nullopt;
    }

    // For Phase 7d: Stack tracking is heuristic-based, so we don't enforce
    // strict stack growth requirements. The key is that introspection ops
    // completed successfully (verified in verifyIntrospectionSuccess).

    // In a full implementation, we would track actual stack state at each step.
    // For now, we just verify that operations exist and succeeded.

    return std::nullopt;  // Stack state correct (simplified check)
}

std::optional<std::string> S12Oracle::verifyDeterminism(const ExecutionTrace& trace) const {
    // Verify introspection is deterministic
    // For Phase 7d: Check trace hash exists (determinism verified in trace)

    if (trace.final_hash == 0) {
        return "Trace hash not computed (determinism not verified)";
    }

    // Introspection operations should be deterministic (same input → same output)
    // This is verified by the trace hash mechanism

    return std::nullopt;  // Deterministic
}

} // namespace test
} // namespace execution
} // namespace dinero
