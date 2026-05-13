#pragma once

#include "taproot_path_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S6Oracle - Hidden Path Non-Activation
 *
 * Property: Unrevealed paths cannot execute
 *
 * Violation conditions:
 * - Script path operations executed without path reveal
 * - Leaf executed without being revealed
 * - Multiple paths activated when only one revealed
 *
 * Observable facts:
 * - Trace contains path_reveals for activated paths
 * - Each LEAF_REVEAL operation corresponds to a path activation
 * - Key path has no path reveals
 * - Script path must have at least one path reveal
 *
 * Pattern: Check that execution only uses revealed paths
 */
class S6Oracle : public TaprootPathOracle {
public:
    std::string getName() const override {
        return "S6: Hidden Path Non-Activation";
    }

protected:
    std::vector<TaprootViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify that unrevealed paths are not activated
     *
     * @return nullopt if safe, error description if violation
     */
    std::optional<std::string> verifyHiddenPathsInactive(const ExecutionTrace& trace) const;

    /**
     * Verify script path has proper reveals
     *
     * @return nullopt if valid, error description if violation
     */
    std::optional<std::string> verifyScriptPathReveals(const ExecutionTrace& trace) const;

    /**
     * Verify key path has no reveals
     *
     * @return nullopt if valid, error description if violation
     */
    std::optional<std::string> verifyKeyPathNoReveals(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
