#pragma once

#include "semantic_determinism_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S22Oracle - Input Permutation Invariance
 *
 * Property: Input ordering does not affect combined result
 *
 * Violation conditions:
 * - Different input orders produce different combined outcomes
 * - Input sequence affects final state
 * - Order-dependent composition
 *
 * Observable facts:
 * - Combined final_hash equality across input permutations
 * - Combined success/failure equality
 * - Multi-input isolation maintained
 *
 * Pattern: Compare traces from different input orderings
 */
class S22Oracle : public SemanticDeterminismOracle {
public:
    std::string getName() const override {
        return "S22: Input Permutation Invariance";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ExecutionTrace>& traces
    ) override;

private:
    /**
     * Verify input permutation invariance
     *
     * @param traces Traces from different input orderings
     * @return nullopt if permutation-invariant, error description if violation
     */
    std::optional<std::string> verifyPermutationInvariance(
        const std::vector<ExecutionTrace>& traces
    ) const;

    /**
     * Verify multi-input structure present
     *
     * @param trace Trace to verify
     * @return true if multi-input, false otherwise
     */
    bool hasMultiInputStructure(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
