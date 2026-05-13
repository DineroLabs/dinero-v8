#pragma once

#include "covenant_semantic_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S11Oracle - Covenant Constraint Enforcement
 *
 * Property: Output spending follows covenant constraints
 *
 * Violation conditions:
 * - Covenant constraints not checked
 * - Constraint validation bypassed
 * - Invalid spending pattern accepted
 *
 * Observable facts:
 * - Covenant operations execute
 * - Introspection returns expected values
 * - Execution succeeds only if constraints satisfied
 *
 * Pattern: Check that covenant constraints are enforced
 */
class S11Oracle : public CovenantSemanticOracle {
public:
    std::string getName() const override {
        return "S11: Covenant Constraint Enforcement";
    }

protected:
    std::vector<CovenantViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify covenant constraints are checked
     *
     * @return nullopt if enforced, error description if violation
     */
    std::optional<std::string> verifyConstraintsEnforced(const ExecutionTrace& trace) const;

    /**
     * Verify introspection operations present
     *
     * @return nullopt if present, error description if missing
     */
    std::optional<std::string> verifyIntrospectionPresent(const ExecutionTrace& trace) const;

    /**
     * Verify covenant validation not bypassed
     *
     * @return nullopt if valid, error description if bypassed
     */
    std::optional<std::string> verifyNotBypassed(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
