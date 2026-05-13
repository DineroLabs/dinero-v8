#pragma once

#include "../framework/execution_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace execution {
namespace test {

/**
 * TaprootViolation - Taproot path safety violation record
 *
 * Describes what Taproot safety rule was violated and where
 */
struct TaprootViolation {
    std::string property_name;  // e.g., "S6: Hidden Path Non-Activation"
    std::string description;    // Human-readable violation description
    uint64_t step;              // Execution step where violation occurred
    std::optional<size_t> leaf_index;  // Leaf involved (if applicable)
    std::string details;        // Additional diagnostic information

    TaprootViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t step_num = 0
    ) : property_name(prop_name)
      , description(desc)
      , step(step_num)
    {}
};

/**
 * TaprootPathOracle - Base class for Taproot path safety property oracles
 *
 * Taproot path safety properties answer: "Are hidden paths safe?"
 * - S6: Hidden Path Non-Activation - Unrevealed paths cannot execute
 * - S7: Partial Reveal Safety - Revealing subset of paths is safe
 * - S8: No Semantic Leakage - Unused leaves don't affect active execution
 * - S9: Path Commitment Completeness - All executable paths are committed
 * - S10: Leaf Execution Uniqueness - Each leaf executes exactly once per input
 *
 * Pattern (following SemanticSafetyOracle):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for violations
 * 3. check(trace) - Public API returning violations
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Return empty vector if no violations, populated vector if violations found
 */
class TaprootPathOracle {
public:
    virtual ~TaprootPathOracle() = default;

    /**
     * Check trace for violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<TaprootViolation> check(const ExecutionTrace& trace) {
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
    virtual std::vector<TaprootViolation> observeTrace(const ExecutionTrace& trace) = 0;

    // Helper methods for Taproot trace analysis
    bool hasTaprootPath(const ExecutionTrace& trace) const;
    bool isKeyPath(const ExecutionTrace& trace) const;
    bool isScriptPath(const ExecutionTrace& trace) const;
    size_t getPathRevealCount(const ExecutionTrace& trace) const;
    std::vector<PathActivation> getPathReveals(const ExecutionTrace& trace) const;
    std::optional<PathActivation> getPathRevealAt(const ExecutionTrace& trace, uint64_t step) const;
    bool hasOperation(const ExecutionTrace& trace, OperationType type) const;
    size_t countOperations(const ExecutionTrace& trace, OperationType type) const;
};

} // namespace test
} // namespace execution
} // namespace dinero
