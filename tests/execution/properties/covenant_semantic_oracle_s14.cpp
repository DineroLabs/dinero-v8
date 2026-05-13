#include "covenant_semantic_oracle_s14.h"
#include <map>

namespace dinero {
namespace execution {
namespace test {

std::vector<CovenantViolation> S14Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CovenantViolation> violations;

    // Only check if recursive covenant present
    if (!hasRecursiveCovenant(trace)) {
        return violations;  // No recursion, property trivially holds
    }

    // Check recursion boundedness
    auto recursion_error = verifyRecursionBounded(trace);
    if (recursion_error) {
        CovenantViolation violation(
            getName(),
            "Recursion boundedness violated: " + *recursion_error,
            0
        );
        violation.details = "Recursive covenant unbounded or infinite";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S14Oracle::verifyRecursionBounded(const ExecutionTrace& trace) const {
    // Check 1: Recursion depth within bounds
    size_t max_depth = getMaxRecursionDepth(trace);

    if (max_depth > MAX_RECURSION_DEPTH) {
        return "Recursion depth " + std::to_string(max_depth) +
               " exceeds limit " + std::to_string(MAX_RECURSION_DEPTH);
    }

    // Check 2: Execution terminates
    auto termination_error = verifyTermination(trace);
    if (termination_error) {
        return termination_error;
    }

    // Check 3: No infinite loops
    auto loop_error = verifyNoInfiniteLoops(trace);
    if (loop_error) {
        return loop_error;
    }

    return std::nullopt;  // Recursion bounded
}

std::optional<std::string> S14Oracle::verifyTermination(const ExecutionTrace& trace) const {
    // Verify execution terminates
    // For Phase 7d: Check that trace has final state

    // Execution should have definite success/failure
    // Trace should not be "stuck" in intermediate state

    if (trace.operation_count == 0 && hasRecursiveCovenant(trace)) {
        return "Execution appears stuck (no operations completed)";
    }

    // Trace should have computed final hash
    if (trace.final_hash == 0) {
        return "Trace not finalized (execution may not have terminated)";
    }

    return std::nullopt;  // Execution terminated
}

std::optional<std::string> S14Oracle::verifyNoInfiniteLoops(const ExecutionTrace& trace) const {
    // Check for infinite loop patterns
    // For Phase 7d: Detect repeated identical operations

    auto introspection_ops = getIntrospectionOps(trace);
    if (introspection_ops.size() < 3) {
        return std::nullopt;  // Not enough ops to form loop
    }

    // Check for repeated operation sequences (potential loop)
    // Simplified: Check if same operation type repeats excessively

    std::map<OperationType, size_t> op_counts;
    for (const auto& op : introspection_ops) {
        op_counts[op.type]++;
    }

    // If any operation type repeats more than MAX_RECURSION_DEPTH times, potential loop
    for (const auto& [type, count] : op_counts) {
        if (count > MAX_RECURSION_DEPTH) {
            return "Operation type appears in loop (count=" + std::to_string(count) + ")";
        }
    }

    return std::nullopt;  // No infinite loops detected
}

} // namespace test
} // namespace execution
} // namespace dinero
