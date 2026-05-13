#pragma once

#include "composition_state_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S17Oracle - Parallel Execution Safety
 *
 * Property: Concurrent script execution is safe
 *
 * Violation conditions:
 * - Race conditions in parallel execution
 * - Non-deterministic parallel outcomes
 * - Unsafe concurrent operations
 *
 * Observable facts:
 * - Parallel operations complete successfully
 * - No data races detected
 * - Deterministic concurrent execution
 *
 * Pattern: Check that parallel execution is safe
 */
class S17Oracle : public CompositionStateOracle {
public:
    std::string getName() const override {
        return "S17: Parallel Execution Safety";
    }

protected:
    std::vector<CompositionViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify parallel execution safety
     *
     * @return nullopt if safe, error description if violation
     */
    std::optional<std::string> verifyParallelSafety(const ExecutionTrace& trace) const;

    /**
     * Verify no data races
     *
     * @return nullopt if no races, error description if violation
     */
    std::optional<std::string> verifyNoDataRaces(const ExecutionTrace& trace) const;

    /**
     * Verify deterministic parallel execution
     *
     * @return nullopt if deterministic, error description if violation
     */
    std::optional<std::string> verifyParallelDeterminism(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
