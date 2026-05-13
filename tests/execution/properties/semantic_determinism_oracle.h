#pragma once

#include "../framework/execution_trace.h"
#include <vector>
#include <string>
#include <optional>

namespace dinero {
namespace execution {
namespace test {

/**
 * DeterminismViolation - Violation of semantic determinism properties
 */
struct DeterminismViolation {
    std::string property_name;
    std::string description;
    uint64_t step;
    std::optional<std::string> trace_comparison;
    std::string details;

    DeterminismViolation(
        const std::string& name,
        const std::string& desc,
        uint64_t s,
        const std::optional<std::string>& comparison = std::nullopt
    ) : property_name(name), description(desc), step(s), trace_comparison(comparison) {}
};

/**
 * SemanticDeterminismOracle - Base class for Phase 7f determinism properties
 *
 * Phase 7f proves that script execution is a pure deterministic function:
 * - S21: Evaluation order doesn't affect outcome
 * - S22: Input permutation doesn't affect combined result
 * - S23: Execution strategy doesn't affect outcome
 * - S24: Syntactic variations produce equivalent results
 * - S25: Full semantic determinism (meta-property)
 *
 * Pattern: Observable-facts-only verification via trace comparison
 */
class SemanticDeterminismOracle {
public:
    virtual ~SemanticDeterminismOracle() = default;

    /**
     * Check property against execution trace(s)
     *
     * @param traces Vector of traces to compare (size depends on property)
     * @return Vector of violations (empty if property holds)
     */
    std::vector<DeterminismViolation> check(const std::vector<ExecutionTrace>& traces) {
        if (traces.empty()) {
            return {DeterminismViolation(
                getName(),
                "No traces provided",
                0
            )};
        }

        auto violations = observeTraces(traces);
        return violations;
    }

    /**
     * Get property name
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Observe traces and detect violations (implemented by each oracle)
     *
     * @param traces Execution traces to compare
     * @return Vector of violations (empty if property holds)
     */
    virtual std::vector<DeterminismViolation> observeTraces(
        const std::vector<ExecutionTrace>& traces
    ) = 0;

    /**
     * Compare two traces for semantic equivalence
     *
     * @param trace1 First trace
     * @param trace2 Second trace
     * @return nullopt if equivalent, error description if different
     */
    std::optional<std::string> compareTraces(
        const ExecutionTrace& trace1,
        const ExecutionTrace& trace2
    ) const;

    /**
     * Verify all traces have same final hash
     *
     * @param traces Traces to verify
     * @return nullopt if all equal, error description if different
     */
    std::optional<std::string> verifyHashEquality(
        const std::vector<ExecutionTrace>& traces
    ) const;

    /**
     * Verify all traces have same success/failure outcome
     *
     * @param traces Traces to verify
     * @return nullopt if all equal, error description if different
     */
    std::optional<std::string> verifyOutcomeEquality(
        const std::vector<ExecutionTrace>& traces
    ) const;

    /**
     * Verify all traces are finalized (have valid hash)
     *
     * @param traces Traces to verify
     * @return nullopt if all finalized, error description if not
     */
    std::optional<std::string> verifyFinalized(
        const std::vector<ExecutionTrace>& traces
    ) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
