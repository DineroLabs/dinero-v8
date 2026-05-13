#pragma once

#include "semantic_safety_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S5Oracle - Script Version Strictness
 *
 * Property: No version downgrade ambiguity
 *
 * Violation conditions:
 * - Script executed with wrong version
 * - Version semantics not enforced
 * - Version downgrade allowed
 *
 * Observable facts:
 * - Script version is recorded (if applicable)
 * - Version-specific rules are enforced
 * - No version ambiguity in execution
 *
 * Pattern: Check single trace for version consistency
 *
 * Phase 7b: Basic invariants (full implementation in future)
 */
class S5Oracle : public SemanticSafetyOracle {
public:
    std::string getName() const override {
        return "S5: Script Version Strictness";
    }

protected:
    std::vector<SemanticViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify script version is consistent
     *
     * @return nullopt if consistent, error description if violation
     */
    std::optional<std::string> verifyVersionConsistency(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
