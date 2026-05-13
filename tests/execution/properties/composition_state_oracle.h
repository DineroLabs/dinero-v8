#pragma once

#include "../framework/execution_trace.h"
#include <vector>
#include <string>
#include <optional>

namespace dinero {
namespace execution {
namespace test {

/**
 * CompositionViolation - Describes a composition/state property violation
 */
struct CompositionViolation {
    std::string property_name;
    std::string description;
    uint64_t step;
    std::optional<size_t> input_index;
    std::string details;

    CompositionViolation(const std::string& name, const std::string& desc, uint64_t s)
        : property_name(name), description(desc), step(s) {}
};

/**
 * CompositionStateOracle - Base class for composition & state property oracles
 *
 * Composition & state properties verify that:
 * - Multiple inputs execute independently
 * - Parallel execution is safe
 * - State updates are consistent
 * - Cross-input invariants hold
 * - Composed execution is deterministic
 *
 * Pattern: Observable-facts-only verification
 */
class CompositionStateOracle {
public:
    virtual ~CompositionStateOracle() = default;

    /**
     * Check if trace violates composition/state properties
     *
     * @param trace Execution trace to verify
     * @return Vector of violations (empty if property holds)
     */
    std::vector<CompositionViolation> check(const ExecutionTrace& trace);

    /**
     * Get human-readable name of this property
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state between checks
     */
    virtual void reset();

    /**
     * Observe trace and detect violations
     *
     * @param trace Execution trace to analyze
     * @return Vector of detected violations
     */
    virtual std::vector<CompositionViolation> observeTrace(const ExecutionTrace& trace) = 0;

    // ========================================================================
    // Helper Methods - Observable Facts Extraction
    // ========================================================================

    /**
     * Check if trace has multiple inputs
     */
    bool hasMultipleInputs(const ExecutionTrace& trace) const;

    /**
     * Get number of inputs in trace
     */
    size_t getInputCount(const ExecutionTrace& trace) const;

    /**
     * Check if trace shows parallel execution
     */
    bool hasParallelExecution(const ExecutionTrace& trace) const;

    /**
     * Check if trace has state updates
     */
    bool hasStateUpdates(const ExecutionTrace& trace) const;

    /**
     * Get operations by type
     */
    std::vector<Operation> getOperationsByType(const ExecutionTrace& trace, OperationType type) const;

    /**
     * Get operation at specific step
     */
    std::optional<Operation> getOperationAtStep(const ExecutionTrace& trace, uint64_t step) const;

    /**
     * Check if execution was successful
     */
    bool wasSuccessful(const ExecutionTrace& trace) const;

    /**
     * Count operations of specific type
     */
    size_t countOperations(const ExecutionTrace& trace, OperationType type) const;

    /**
     * Check if operations are isolated (don't interfere)
     */
    bool areOperationsIsolated(const ExecutionTrace& trace) const;

    /**
     * Get maximum concurrent operations
     */
    size_t getMaxConcurrentOps(const ExecutionTrace& trace) const;

    /**
     * Check if trace is deterministic
     */
    bool isDeterministic(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
