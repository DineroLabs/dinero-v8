#pragma once

#include "composition_state_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S18Oracle - State Consistency
 *
 * Property: State updates are consistent across composition
 *
 * Violation conditions:
 * - Inconsistent state updates
 * - State corruption during composition
 * - Lost state updates
 *
 * Observable facts:
 * - All state updates recorded
 * - State transitions are valid
 * - Final state is consistent
 *
 * Pattern: Check that state remains consistent
 */
class S18Oracle : public CompositionStateOracle {
public:
    std::string getName() const override {
        return "S18: State Consistency";
    }

protected:
    std::vector<CompositionViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify state consistency
     *
     * @return nullopt if consistent, error description if violation
     */
    std::optional<std::string> verifyStateConsistency(const ExecutionTrace& trace) const;

    /**
     * Verify all updates recorded
     *
     * @return nullopt if recorded, error description if violation
     */
    std::optional<std::string> verifyUpdatesRecorded(const ExecutionTrace& trace) const;

    /**
     * Verify final state validity
     *
     * @return nullopt if valid, error description if violation
     */
    std::optional<std::string> verifyFinalStateValid(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
