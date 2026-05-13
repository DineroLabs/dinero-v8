#pragma once

#include "semantic_safety_oracle.h"

namespace dinero {
namespace execution {
namespace test {

/**
 * S2Oracle - No Alternate Witness Equivalence
 *
 * Property: Different witnesses → different execution paths
 *
 * Violation conditions:
 * - Witness data exists but is not used in execution
 * - Execution produces same result independent of witness content
 * - No witness-dependent operations recorded
 *
 * Observable facts:
 * - Trace contains witness elements
 * - Witness elements are pushed to stack
 * - Stack operations use witness-derived values
 * - Final result depends on witness data
 *
 * Pattern: Check single trace for witness utilization
 *
 * Note: Full witness uniqueness testing (comparing two traces with different witnesses)
 * is done in the property test. The oracle verifies witness is actually used.
 */
class S2Oracle : public SemanticSafetyOracle {
public:
    std::string getName() const override {
        return "S2: No Alternate Witness Equivalence";
    }

protected:
    std::vector<SemanticViolation> observeTrace(const ExecutionTrace& trace) override;

private:
    /**
     * Check if witness is properly utilized
     *
     * @return nullopt if witness used, error description if ignored
     */
    std::optional<std::string> verifyWitnessUtilization(const ExecutionTrace& trace) const;

    /**
     * Check if witness elements are pushed to stack
     *
     * @return nullopt if witness pushed, error description if not
     */
    std::optional<std::string> verifyWitnessPushed(const ExecutionTrace& trace) const;

    /**
     * Check if stack operations use witness data
     *
     * @return nullopt if witness used in operations, error description if not
     */
    std::optional<std::string> verifyWitnessInOperations(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
