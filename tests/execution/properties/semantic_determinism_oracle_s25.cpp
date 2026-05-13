#include "semantic_determinism_oracle_s25.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<DeterminismViolation> S25Oracle::observeTraces(
    const std::vector<ExecutionTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    // S25 is a meta-property - it verifies infrastructure, not specific comparisons
    // The actual S21-S24 testing happens in test suite
    // Here we just verify traces have determinism markers

    for (size_t i = 0; i < traces.size(); ++i) {
        auto marker_error = verifyDeterminismMarkers(traces[i]);
        if (marker_error) {
            violations.push_back(DeterminismViolation(
                getName(),
                "Trace " + std::to_string(i) + " missing determinism markers: " + *marker_error,
                0
            ));
        }
    }

    // If multiple traces provided, verify full determinism
    if (traces.size() > 1 && violations.empty()) {
        auto determinism_error = verifyFullDeterminism(traces);
        if (determinism_error) {
            violations.push_back(DeterminismViolation(
                getName(),
                "Full determinism violated: " + *determinism_error,
                0
            ));
            violations.back().details = "Execution is not fully deterministic";
        }
    }

    return violations;
}

std::optional<std::string> S25Oracle::verifyFullDeterminism(
    const std::vector<ExecutionTrace>& traces
) const {
    // Verify all traces finalized
    auto finalized_error = verifyFinalized(traces);
    if (finalized_error) {
        return finalized_error;
    }

    // For S25, if multiple traces provided, they should be identical
    // (This represents replaying the same execution with same RNG seed)
    auto hash_error = verifyHashEquality(traces);
    if (hash_error) {
        return hash_error;
    }

    auto outcome_error = verifyOutcomeEquality(traces);
    if (outcome_error) {
        return outcome_error;
    }

    return std::nullopt;  // Fully deterministic
}

std::optional<std::string> S25Oracle::verifyDeterminismMarkers(
    const ExecutionTrace& trace
) const {
    // Check 1: Trace finalized (has hash)
    if (trace.final_hash == 0) {
        return "Trace not finalized (missing final_hash)";
    }

    // Check 2: RNG seed present (for reproducibility)
    if (trace.rng_seed == 0) {
        return "No RNG seed (execution may be non-deterministic)";
    }

    // Check 3: Trace has definite outcome
    // (success flag should be meaningful)
    // This is always true for ExecutionTrace structure

    return std::nullopt;  // Has determinism markers
}

} // namespace test
} // namespace execution
} // namespace dinero
