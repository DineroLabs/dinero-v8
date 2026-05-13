#include "semantic_determinism_oracle_s23.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<DeterminismViolation> S23Oracle::observeTraces(
    const std::vector<ExecutionTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    // S23 requires at least 2 traces (different execution strategies)
    if (traces.size() < 2) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Need at least 2 traces to verify strategy independence",
            0
        ));
        return violations;
    }

    // Check all traces finalized
    auto finalized_error = verifyFinalized(traces);
    if (finalized_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Strategy independence check failed: " + *finalized_error,
            0
        ));
        return violations;
    }

    // Check strategy independence
    auto strategy_error = verifyStrategyIndependence(traces);
    if (strategy_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Execution strategy affected outcome: " + *strategy_error,
            0
        ));
        violations.back().details = "Different execution strategies produced different results";
    }

    return violations;
}

std::optional<std::string> S23Oracle::verifyStrategyIndependence(
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
            return "Strategy " + std::to_string(i) + ": " + *comparison_error;
        }
    }

    return std::nullopt;  // Strategy-independent
}

} // namespace test
} // namespace execution
} // namespace dinero
