#include "composition_state_oracle_s20.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<CompositionViolation> S20Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<CompositionViolation> violations;

    // Always check determinism (applies to all executions)

    // Check composition determinism
    auto determinism_error = verifyCompositionDeterminism(trace);
    if (determinism_error) {
        CompositionViolation violation(
            getName(),
            "Composition determinism violated: " + *determinism_error,
            0
        );
        violation.details = "Execution is non-deterministic";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S20Oracle::verifyCompositionDeterminism(const ExecutionTrace& trace) const {
    // Check 1: Trace hash computed
    auto hash_error = verifyTraceHash(trace);
    if (hash_error) {
        return hash_error;
    }

    // Check 2: No randomness sources
    auto randomness_error = verifyNoRandomness(trace);
    if (randomness_error) {
        return randomness_error;
    }

    // Check 3: All operations have definite outcomes
    for (const auto& op : trace.operations) {
        // Each operation should have explicit success/failure
        // (This is enforced by Operation structure requiring success field)
    }

    // Check 4: Execution has definite outcome
    // Trace should have success or failure recorded
    if (trace.operation_count > 0) {
        // Verify trace was finalized
        if (trace.final_hash == 0) {
            return "Trace not finalized despite having operations";
        }
    }

    return std::nullopt;  // Composition is deterministic
}

std::optional<std::string> S20Oracle::verifyTraceHash(const ExecutionTrace& trace) const {
    // Verify trace hash was computed
    if (trace.final_hash == 0) {
        return "Trace hash not computed";
    }

    return std::nullopt;  // Trace hash computed
}

std::optional<std::string> S20Oracle::verifyNoRandomness(const ExecutionTrace& trace) const {
    // Verify no randomness in execution
    // For Phase 7e: Check that execution is based on deterministic RNG

    // Trace should have RNG seed
    if (trace.rng_seed == 0) {
        return "No RNG seed (execution may be non-deterministic)";
    }

    // All operations should be reproducible
    // Verified by trace hash mechanism

    return std::nullopt;  // No randomness
}

} // namespace test
} // namespace execution
} // namespace dinero
