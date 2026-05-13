#pragma once

#include "taproot_path_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S8Oracle - No Semantic Leakage from Unused Leaves
 *
 * Property: Unused leaves don't affect active execution
 *
 * Violation conditions:
 * - Unrevealed leaf influences execution
 * - Hidden path data leaks into active path
 * - Execution depends on unrevealed paths
 *
 * Observable facts:
 * - Only revealed paths affect execution
 * - Hidden leaves are completely isolated
 * - No information leakage from structure
 *
 * Pattern: Check that unrevealed paths don't affect execution
 */
class S8Oracle : public TaprootPathOracle {
public:
    std::string getName() const override {
        return "S8: No Semantic Leakage from Unused Leaves";
    }

protected:
    std::vector<TaprootViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify no leakage from unrevealed paths
     *
     * @return nullopt if safe, error description if violation
     */
    std::optional<std::string> verifyNoLeakage(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
