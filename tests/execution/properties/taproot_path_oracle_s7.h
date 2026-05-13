#pragma once

#include "taproot_path_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S7Oracle - Partial Reveal Safety
 *
 * Property: Revealing subset of paths is safe
 *
 * Violation conditions:
 * - Revealing one path enables unrevealed paths
 * - Partial reveal changes semantics of other paths
 * - Multiple reveals interfere with each other
 *
 * Observable facts:
 * - Each path reveal is independent
 * - Revealing path A doesn't affect path B
 * - Partial reveals don't compromise security
 *
 * Pattern: Check that partial reveals are isolated
 */
class S7Oracle : public TaprootPathOracle {
public:
    std::string getName() const override {
        return "S7: Partial Reveal Safety";
    }

protected:
    std::vector<TaprootViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify path reveals are independent
     *
     * @return nullopt if safe, error description if violation
     */
    std::optional<std::string> verifyRevealIndependence(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
