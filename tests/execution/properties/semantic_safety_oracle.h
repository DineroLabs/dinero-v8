#pragma once

#include "../framework/execution_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace execution {
namespace test {

/**
 * SemanticViolation - Semantic safety property violation record
 *
 * Describes what semantic rule was violated and where
 */
struct SemanticViolation {
    std::string property_name;  // e.g., "S1: Script Determinism"
    std::string description;    // Human-readable violation description
    uint64_t step;              // Execution step where violation occurred
    std::optional<std::string> operation;  // Operation involved (if applicable)
    std::string details;        // Additional diagnostic information

    SemanticViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t step_num = 0
    ) : property_name(prop_name)
      , description(desc)
      , step(step_num)
    {}
};

/**
 * SemanticSafetyOracle - Base class for semantic safety property oracles
 *
 * Safety properties answer: "Can execution have multiple meanings?"
 * - S1: Script Determinism - Same inputs → same result
 * - S2: No Alternate Witness Equivalence - Different witnesses → different execution paths
 * - S3: Taproot Leaf Isolation - Revealing one leaf doesn't enable others
 * - S4: Key-Path ≠ Script-Path Semantics - Distinct execution paths have distinct meanings
 * - S5: Script Version Strictness - No version downgrade ambiguity
 *
 * Pattern (following Ring 6's EconomicSafetyOracle):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for violations
 * 3. check(trace) - Public API returning violations
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Return empty vector if no violations, populated vector if violations found
 */
class SemanticSafetyOracle {
public:
    virtual ~SemanticSafetyOracle() = default;

    /**
     * Check trace for violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<SemanticViolation> check(const ExecutionTrace& trace) {
        reset();
        return observeTrace(trace);
    }

    /**
     * Get oracle name (for reporting)
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state before analyzing new trace
     *
     * Subclasses override to clear property-specific state
     */
    virtual void reset() {
        // Default: no state to clear
    }

    /**
     * Observe trace and detect violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<SemanticViolation> observeTrace(const ExecutionTrace& trace) = 0;

    // Helper methods for trace analysis
    std::vector<Operation> getOperationsByType(const ExecutionTrace& trace, OperationType type) const;
    std::optional<Operation> getOperationAtStep(const ExecutionTrace& trace, uint64_t step) const;
    std::vector<StackSnapshot> getStackSnapshots(const ExecutionTrace& trace) const;
    std::optional<StackSnapshot> getStackAtStep(const ExecutionTrace& trace, uint64_t step) const;
    bool wasSuccessful(const ExecutionTrace& trace) const;
    bool hasFailed(const ExecutionTrace& trace) const;
    std::optional<std::string> getError(const ExecutionTrace& trace) const;
    uint64_t getOperationCount(const ExecutionTrace& trace) const;
    uint64_t getMaxStackDepth(const ExecutionTrace& trace) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
