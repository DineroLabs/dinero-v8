#include "semantic_determinism_oracle_s24.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<DeterminismViolation> S24Oracle::observeTraces(
    const std::vector<ExecutionTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    // S24 requires at least 2 traces (original + canonical/equivalent form)
    if (traces.size() < 2) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Need at least 2 traces to verify canonical equivalence",
            0
        ));
        return violations;
    }

    // Check all traces finalized
    auto finalized_error = verifyFinalized(traces);
    if (finalized_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Canonical equivalence check failed: " + *finalized_error,
            0
        ));
        return violations;
    }

    // Check canonical equivalence
    auto equivalence_error = verifyCanonicalEquivalence(traces);
    if (equivalence_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Canonical forms not equivalent: " + *equivalence_error,
            0
        ));
        violations.back().details = "Syntactically different scripts produced different results";
    }

    return violations;
}

std::optional<std::string> S24Oracle::verifyCanonicalEquivalence(
    const std::vector<ExecutionTrace>& traces
) const {
    // Verify hash equality (strongest check for semantic equivalence)
    auto hash_error = verifyHashEquality(traces);
    if (hash_error) {
        return hash_error;
    }

    // Verify outcome equality
    auto outcome_error = verifyOutcomeEquality(traces);
    if (outcome_error) {
        return outcome_error;
    }

    // Pairwise comparison
    for (size_t i = 1; i < traces.size(); ++i) {
        auto comparison_error = compareTraces(traces[0], traces[i]);
        if (comparison_error) {
            return "Variant " + std::to_string(i) + ": " + *comparison_error;
        }
    }

    // NOTE: We do NOT require operation_count equality
    // Equivalent scripts may have different step counts
    // Example: OP_1 OP_1 OP_ADD (3 steps) vs OP_2 (1 step)
    // Both are semantically equivalent but different costs

    return std::nullopt;  // Canonically equivalent
}

} // namespace test
} // namespace execution
} // namespace dinero
