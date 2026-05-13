#pragma once

#include "covenant_semantic_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S13Oracle - Covenant Composition Safety
 *
 * Property: Multiple covenant constraints compose correctly
 *
 * Violation conditions:
 * - Multiple covenants interfere with each other
 * - Covenant composition creates contradictions
 * - Composed covenants violate individual constraints
 *
 * Observable facts:
 * - Multiple covenant operations execute
 * - Each covenant constraint checked independently
 * - Composition doesn't bypass individual checks
 *
 * Pattern: Check that covenant composition is safe
 */
class S13Oracle : public CovenantSemanticOracle {
public:
    std::string getName() const override {
        return "S13: Covenant Composition Safety";
    }

protected:
    std::vector<CovenantViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify multiple covenants don't interfere
     *
     * @return nullopt if safe, error description if violation
     */
    std::optional<std::string> verifyNoInterference(const ExecutionTrace& trace) const;

    /**
     * Verify each covenant constraint checked
     *
     * @return nullopt if checked, error description if violation
     */
    std::optional<std::string> verifyAllChecked(const ExecutionTrace& trace) const;

    /**
     * Verify composition doesn't bypass constraints
     *
     * @return nullopt if safe, error description if violation
     */
    std::optional<std::string> verifyNoBypass(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
