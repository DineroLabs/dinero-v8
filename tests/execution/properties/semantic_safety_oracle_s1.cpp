#include "semantic_safety_oracle_s1.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<SemanticViolation> S1Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<SemanticViolation> violations;

    // Check 1: Trace is well-formed
    auto well_formed_error = verifyWellFormed(trace);
    if (well_formed_error) {
        SemanticViolation violation(
            getName(),
            "Trace not well-formed: " + *well_formed_error,
            0
        );
        violation.details = "Scenario: " + trace.scenario_name +
                           ", Seed: " + std::to_string(trace.rng_seed);
        violations.push_back(violation);
        return violations;  // Don't continue if not well-formed
    }

    // Check 2: Final hash is correct
    auto hash_error = verifyHash(trace);
    if (hash_error) {
        SemanticViolation violation(
            getName(),
            "Hash verification failed: " + *hash_error,
            trace.operation_count
        );
        violation.details = "Expected hash to match recomputed value";
        violations.push_back(violation);
    }

    // Check 3: No non-deterministic behavior
    auto determinism_error = checkDeterminism(trace);
    if (determinism_error) {
        SemanticViolation violation(
            getName(),
            "Non-deterministic behavior: " + *determinism_error,
            trace.operation_count
        );
        violation.details = "Trace shows signs of non-deterministic execution";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S1Oracle::verifyWellFormed(const ExecutionTrace& trace) const {
    // Check operation count matches recorded operations
    if (trace.operation_count != trace.operations.size()) {
        return "Operation count mismatch (count=" +
               std::to_string(trace.operation_count) +
               ", actual=" + std::to_string(trace.operations.size()) + ")";
    }

    // Check operations are ordered by step
    for (size_t i = 1; i < trace.operations.size(); i++) {
        if (trace.operations[i].step <= trace.operations[i-1].step) {
            return "Operations not in step order at index " + std::to_string(i);
        }
    }

    // Check stack snapshots are ordered by step
    for (size_t i = 1; i < trace.stack_states.size(); i++) {
        if (trace.stack_states[i].step <= trace.stack_states[i-1].step) {
            return "Stack snapshots not in step order at index " + std::to_string(i);
        }
    }

    // Check max stack depth is valid
    size_t actual_max_depth = 0;
    for (const auto& snapshot : trace.stack_states) {
        if (snapshot.depth > actual_max_depth) {
            actual_max_depth = snapshot.depth;
        }
    }
    if (trace.stack_depth_max != actual_max_depth) {
        return "Max stack depth mismatch (recorded=" +
               std::to_string(trace.stack_depth_max) +
               ", actual=" + std::to_string(actual_max_depth) + ")";
    }

    return std::nullopt;  // Well-formed
}

std::optional<std::string> S1Oracle::verifyHash(const ExecutionTrace& trace) const {
    // Recompute hash and compare
    uint64_t recomputed = trace.computeHash();

    if (recomputed != trace.final_hash) {
        return "Hash mismatch (stored=" +
               std::to_string(trace.final_hash) +
               ", recomputed=" + std::to_string(recomputed) + ")";
    }

    return std::nullopt;  // Hash valid
}

std::optional<std::string> S1Oracle::checkDeterminism(const ExecutionTrace& trace) const {
    // Check 1: If trace failed, must have error message
    if (!trace.success && !trace.error) {
        return "Failed execution without error message";
    }

    // Check 2: If trace succeeded, must not have error message
    if (trace.success && trace.error) {
        return "Successful execution with error message: " + *trace.error;
    }

    // Check 3: All operations must have consistent success flags
    for (const auto& op : trace.operations) {
        // If an operation failed, the overall trace should fail
        if (!op.success && trace.success) {
            return "Operation failed but trace succeeded (step " +
                   std::to_string(op.step) + ")";
        }

        // Failed operation must have error
        if (!op.success && !op.error) {
            return "Failed operation without error (step " +
                   std::to_string(op.step) + ")";
        }
    }

    // Check 4: Final hash must be non-zero (zero would indicate uninitialized)
    if (trace.final_hash == 0) {
        return "Final hash is zero (uninitialized)";
    }

    return std::nullopt;  // Deterministic
}

} // namespace test
} // namespace execution
} // namespace dinero
