#pragma once

#include "semantic_determinism_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S23Oracle - Strategy Independence
 *
 * Property: Execution strategy does not affect semantic outcome
 *
 * Strategies tested:
 * - Eager vs lazy evaluation
 * - Different optimization levels
 * - Stack-based vs alternative execution models
 *
 * Violation conditions:
 * - Different strategies produce different results
 * - Strategy-dependent optimization changes outcome
 * - Implementation leakage into semantics
 *
 * Observable facts:
 * - final_hash equality across strategies
 * - final_state equality across strategies
 * - success/failure outcome equality
 *
 * Pattern: Compare traces from different execution strategies
 */
class S23Oracle : public SemanticDeterminismOracle {
public:
    std::string getName() const override {
        return "S23: Strategy Independence";
    }

protected:
    std::vector<DeterminismViolation> observeTraces(
        const std::vector<ExecutionTrace>& traces
    ) override;

private:
    /**
     * Verify strategy independence
     *
     * @param traces Traces from different execution strategies
     * @return nullopt if strategy-independent, error description if violation
     */
    std::optional<std::string> verifyStrategyIndependence(
        const std::vector<ExecutionTrace>& traces
    ) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
