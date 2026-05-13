#pragma once

#include "covenant_semantic_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S15Oracle - Covenant State Transitions
 *
 * Property: State machine transitions follow covenant rules
 *
 * Violation conditions:
 * - Invalid state transition executed
 * - State transition skips validation
 * - State machine enters invalid state
 *
 * Observable facts:
 * - State transitions checked
 * - Introspection validates state
 * - Execution succeeds only if transition valid
 *
 * Pattern: Check that state transitions are valid
 */
class S15Oracle : public CovenantSemanticOracle {
public:
    std::string getName() const override {
        return "S15: Covenant State Transitions";
    }

protected:
    std::vector<CovenantViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify state transitions validated
     *
     * @return nullopt if valid, error description if violation
     */
    std::optional<std::string> verifyTransitionsValidated(const ExecutionTrace& trace) const;

    /**
     * Verify state transition order
     *
     * @return nullopt if valid, error description if violation
     */
    std::optional<std::string> verifyTransitionOrder(const ExecutionTrace& trace) const;

    /**
     * Verify no invalid state reached
     *
     * @return nullopt if valid, error description if violation
     */
    std::optional<std::string> verifyNoInvalidState(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
