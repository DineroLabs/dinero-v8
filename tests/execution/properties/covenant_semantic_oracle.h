#pragma once

#include "../framework/execution_trace.h"
#include <vector>
#include <string>
#include <optional>

namespace dinero {
namespace execution {
namespace test {

/**
 * CovenantViolation - Describes a covenant semantic property violation
 */
struct CovenantViolation {
    std::string property_name;
    std::string description;
    uint64_t step;
    std::optional<std::string> operation;
    std::string details;

    CovenantViolation(const std::string& name, const std::string& desc, uint64_t s)
        : property_name(name), description(desc), step(s) {}
};

/**
 * CovenantSemanticOracle - Base class for covenant semantic property oracles
 *
 * Covenant semantics verify that:
 * - Output spending follows covenant constraints
 * - Introspection opcodes return correct values
 * - Covenant composition is safe
 * - Recursive covenants are bounded
 * - State transitions follow covenant rules
 *
 * Pattern: Observable-facts-only verification
 */
class CovenantSemanticOracle {
public:
    virtual ~CovenantSemanticOracle() = default;

    /**
     * Check if trace violates covenant semantic properties
     *
     * @param trace Execution trace to verify
     * @return Vector of violations (empty if property holds)
     */
    std::vector<CovenantViolation> check(const ExecutionTrace& trace);

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
    virtual std::vector<CovenantViolation> observeTrace(const ExecutionTrace& trace) = 0;

    // ========================================================================
    // Helper Methods - Observable Facts Extraction
    // ========================================================================

    /**
     * Check if trace contains covenant operations
     */
    bool hasCovenantOps(const ExecutionTrace& trace) const;

    /**
     * Get all introspection operations (OP_CHECKSIGFROMSTACK, etc.)
     */
    std::vector<Operation> getIntrospectionOps(const ExecutionTrace& trace) const;

    /**
     * Check if trace uses recursive covenant pattern
     */
    bool hasRecursiveCovenant(const ExecutionTrace& trace) const;

    /**
     * Count covenant operations in trace
     */
    size_t countCovenantOps(const ExecutionTrace& trace) const;

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
     * Get stack size at step
     */
    size_t getStackSizeAtStep(const ExecutionTrace& trace, uint64_t step) const;

    /**
     * Count operations of specific type
     */
    size_t countOperations(const ExecutionTrace& trace, OperationType type) const;

    /**
     * Check if trace has state transitions
     */
    bool hasStateTransitions(const ExecutionTrace& trace) const;

    /**
     * Get maximum recursion depth
     */
    size_t getMaxRecursionDepth(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
