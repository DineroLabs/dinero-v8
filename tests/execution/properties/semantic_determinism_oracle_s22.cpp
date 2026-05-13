#include "semantic_determinism_oracle_s22.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<DeterminismViolation> S22Oracle::observeTraces(
    const std::vector<ExecutionTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    // S22 requires at least 2 traces (different input orderings)
    if (traces.size() < 2) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Need at least 2 traces to verify input permutation invariance",
            0
        ));
        return violations;
    }

    // Check all traces finalized
    auto finalized_error = verifyFinalized(traces);
    if (finalized_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Input permutation check failed: " + *finalized_error,
            0
        ));
        return violations;
    }

    // Verify traces have multi-input structure
    if (!hasMultiInputStructure(traces[0])) {
        // Property trivially holds for single-input scripts
        return violations;
    }

    // Check permutation invariance
    auto invariance_error = verifyPermutationInvariance(traces);
    if (invariance_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Input permutation affected outcome: " + *invariance_error,
            0
        ));
        violations.back().details = "Different input orderings produced different results";
    }

    return violations;
}

std::optional<std::string> S22Oracle::verifyPermutationInvariance(
    const std::vector<ExecutionTrace>& traces
) const {
    // Verify hash equality
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
            return "Permutation " + std::to_string(i) + ": " + *comparison_error;
        }
    }

    return std::nullopt;  // Permutation-invariant
}

bool S22Oracle::hasMultiInputStructure(const ExecutionTrace& trace) const {
    // Check for multi-input markers in events
    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::MULTI_INPUT_START) {
            return true;
        }
    }
    return false;
}

} // namespace test
} // namespace execution
} // namespace dinero
