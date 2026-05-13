#pragma once

#include "covenant_semantic_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S14Oracle - Covenant Recursion Boundedness
 *
 * Property: Recursive covenants terminate (don't loop infinitely)
 *
 * Violation conditions:
 * - Unbounded recursion detected
 * - Recursion depth exceeds limits
 * - Infinite loop in covenant chain
 *
 * Observable facts:
 * - Recursion depth stays within bounds
 * - Execution terminates
 * - No infinite recursion patterns
 *
 * Pattern: Check that recursive covenants are bounded
 */
class S14Oracle : public CovenantSemanticOracle {
public:
    std::string getName() const override {
        return "S14: Covenant Recursion Boundedness";
    }

protected:
    std::vector<CovenantViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Verify recursion depth within bounds
     *
     * @return nullopt if bounded, error description if violation
     */
    std::optional<std::string> verifyRecursionBounded(const ExecutionTrace& trace) const;

    /**
     * Verify execution terminates
     *
     * @return nullopt if terminates, error description if violation
     */
    std::optional<std::string> verifyTermination(const ExecutionTrace& trace) const;

    /**
     * Verify no infinite loops
     *
     * @return nullopt if no loops, error description if violation
     */
    std::optional<std::string> verifyNoInfiniteLoops(const ExecutionTrace& trace) const;

    /**
     * Maximum allowed recursion depth
     */
    static constexpr size_t MAX_RECURSION_DEPTH = 100;
};

} // namespace test
} // namespace execution
} // namespace dinero
