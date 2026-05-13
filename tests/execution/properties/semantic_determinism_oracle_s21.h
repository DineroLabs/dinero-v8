#pragma once

#include "semantic_determinism_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S21Oracle - Evaluation Order Determinism
 *
 * Property: Evaluation order does not affect semantic outcome
 *
 * Violation conditions:
 * - Different evaluation orders produce different results
 * - Order-dependent side effects
 * - Non-commutative operations producing different outcomes
 *
 * Observable facts:
 * - final_hash equality across orders
 * - final_state equality across orders
 * - success/failure outcome equality
 *
 * Pattern: Compare traces from different evaluation orders
 */
class S21Oracle : public SemanticDeterminismOracle {
public:
    std::string getName() const override {
        return "S21: Evaluation Order Determinism";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ExecutionTrace>& traces
    ) override;

private:
    /**
     * Verify evaluation order independence
     *
     * @param traces Traces from different evaluation orders
     * @return nullopt if order-independent, error description if violation
     */
    std::optional<std::string> verifyOrderIndependence(
        const std::vector<ExecutionTrace>& traces
    ) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
