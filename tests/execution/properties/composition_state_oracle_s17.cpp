#include "composition_state_oracle_s17.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CompositionViolation> S17Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CompositionViolation> violations;

    // Only check if parallel execution present
    if (!hasParallelExecution(trace)) {
        return violations;  // No parallel execution, property trivially holds
    }

    // Check parallel safety
    auto safety_error = verifyParallelSafety(trace);
    if (safety_error) {
        CompositionViolation violation(
            getName(),
            "Parallel execution safety violated: " + *safety_error,
            0
        );
        violation.details = "Concurrent execution is unsafe";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S17Oracle::verifyParallelSafety(const ExecutionTrace& trace) const {
    // Check 1: No data races
    auto race_error = verifyNoDataRaces(trace);
    if (race_error) {
        return race_error;
    }

    // Check 2: Deterministic parallel execution
    auto determinism_error = verifyParallelDeterminism(trace);
    if (determinism_error) {
        return determinism_error;
    }

    // Check 3: All concurrent operations succeeded or all failed consistently
    size_t max_concurrent = getMaxConcurrentOps(trace);
    if (max_concurrent > 1) {
        // Verify concurrent operations have consistent outcomes
        uint64_t current_step = trace.operations[0].step;
        bool first_success = true;
        bool has_first = false;

        for (const auto& op : trace.operations) {
            if (op.step == current_step) {
                if (!has_first) {
                    first_success = op.success;
                    has_first = true;
                } else {
                    // All concurrent ops should have same success status
                    if (op.success != first_success) {
                        return "Inconsistent concurrent operation outcomes at step " +
                               std::to_string(current_step);
                    }
                }
            } else {
                current_step = op.step;
                has_first = false;
            }
        }
    }

    return std::nullopt;  // Parallel execution safe
}

std::optional<std::string> S17Oracle::verifyNoDataRaces(const ExecutionTrace& trace) const {
    // Verify no data races in parallel execution
    // For Phase 7e: Check that trace is deterministic (no races)

    if (!isDeterministic(trace)) {
        return "Non-deterministic trace suggests data races";
    }

    return std::nullopt;  // No data races
}

std::optional<std::string> S17Oracle::verifyParallelDeterminism(const ExecutionTrace& trace) const {
    // Verify parallel execution is deterministic
    // For Phase 7e: Check trace hash computed

    if (trace.final_hash == 0) {
        return "Trace hash not computed (parallel determinism not verified)";
    }

    return std::nullopt;  // Deterministic
}

} // namespace test
} // namespace execution
} // namespace dinero
