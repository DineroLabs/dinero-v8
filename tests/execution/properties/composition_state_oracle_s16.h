#pragma once

#include "composition_state_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S16Oracle - Multi-Input Isolation
 *
 * Property: Inputs execute independently without interference
 *
 * Violation conditions:
 * - One input affects another's execution
 * - Shared state between inputs
 * - Cross-input data leakage
 *
 * Observable facts:
 * - Each input marked as isolated
 * - No cross-input operations
 * - Independent execution traces
 *
 * Pattern: Check that multiple inputs don't interfere
 */
class S16Oracle : public CompositionStateOracle {
public:
    std::string getName() const override {
        return "S16: Multi-Input Isolation";
    }

protected:
    std::vector<CompositionViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify inputs are isolated
     *
     * @return nullopt if isolated, error description if violation
     */
    std::optional<std::string> verifyInputsIsolated(const ExecutionTrace& trace) const;

    /**
     * Verify no cross-input operations
     *
     * @return nullopt if no cross-ops, error description if violation
     */
    std::optional<std::string> verifyNoCrossInputOps(const ExecutionTrace& trace) const;

    /**
     * Verify independent execution
     *
     * @return nullopt if independent, error description if violation
     */
    std::optional<std::string> verifyIndependentExecution(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
