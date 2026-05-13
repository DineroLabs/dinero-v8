#pragma once

#include "taproot_path_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S10Oracle - Leaf Execution Uniqueness
 *
 * Property: Each leaf executes exactly once per input
 *
 * Violation conditions:
 * - Same leaf executed multiple times
 * - Leaf execution replayed
 * - Multiple activations of same path
 *
 * Observable facts:
 * - Each leaf index appears at most once in path reveals
 * - No duplicate leaf executions
 * - Path activation is unique
 *
 * Pattern: Check that each leaf executes only once
 */
class S10Oracle : public TaprootPathOracle {
public:
    std::string getName() const override {
        return "S10: Leaf Execution Uniqueness";
    }

protected:
    std::vector<TaprootViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify each leaf executes exactly once
     *
     * @return nullopt if unique, error description if violation
     */
    std::optional<std::string> verifyLeafUniqueness(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
