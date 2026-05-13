#include "semantic_determinism_oracle_s21.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<DeterminismViolation> S21Oracle::observeTraces(
    const std::vector<ExecutionTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    // S21 requires at least 2 traces (different evaluation orders)
    if (traces.size() < 2) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Need at least 2 traces to verify evaluation order independence",
            0
        ));
        return violations;
    }

    // Check all traces finalized
    auto finalized_error = verifyFinalized(traces);
    if (finalized_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Evaluation order check failed: " + *finalized_error,
            0
        ));
        return violations;
    }

    // Check order independence
    auto order_error = verifyOrderIndependence(traces);
    if (order_error) {
        violations.push_back(DeterminismViolation(
            getName(),
            "Evaluation order affected outcome: " + *order_error,
            0
        ));
        violations.back().details = "Different evaluation orders produced different results";
    }

    return violations;
}

std::optional<std::string> S21Oracle::verifyOrderIndependence(
    const std::vector<ExecutionTrace>& traces
) const {
    // Verify hash equality (strongest check)
    auto hash_error = verifyHashEquality(traces);
    if (hash_error) {
        return hash_error;
    }

    // Verify outcome equality
    auto outcome_error = verifyOutcomeEquality(traces);
    if (outcome_error) {
        return outcome_error;
    }

    // Pairwise trace comparison for detailed check
    for (size_t i = 1; i < traces.size(); ++i) {
        auto comparison_error = compareTraces(traces[0], traces[i]);
        if (comparison_error) {
            return "Order " + std::to_string(i) + ": " + *comparison_error;
        }
    }

    return std::nullopt;  // Order-independent
}

} // namespace test
} // namespace execution
} // namespace dinero
