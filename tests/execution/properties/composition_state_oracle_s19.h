#pragma once

#include "composition_state_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S19Oracle - Cross-Input Invariants
 *
 * Property: Invariants hold across multiple inputs
 *
 * Violation conditions:
 * - Invariant violated in multi-input scenario
 * - Cross-input constraints not enforced
 * - Global invariants broken
 *
 * Observable facts:
 * - Invariants checked for each input
 * - Cross-input constraints verified
 * - Global state remains valid
 *
 * Pattern: Check that invariants hold across composition
 */
class S19Oracle : public CompositionStateOracle {
public:
    std::string getName() const override {
        return "S19: Cross-Input Invariants";
    }

protected:
    std::vector<CompositionViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify cross-input invariants
     *
     * @return nullopt if hold, error description if violation
     */
    std::optional<std::string> verifyCrossInputInvariants(const ExecutionTrace& trace) const;

    /**
     * Verify per-input invariants
     *
     * @return nullopt if hold, error description if violation
     */
    std::optional<std::string> verifyPerInputInvariants(const ExecutionTrace& trace) const;

    /**
     * Verify global invariants
     *
     * @return nullopt if hold, error description if violation
     */
    std::optional<std::string> verifyGlobalInvariants(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
