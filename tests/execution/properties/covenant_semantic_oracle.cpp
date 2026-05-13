#include "covenant_semantic_oracle.h"
#include <algorithm>

namespace dinero {
namespace execution {
namespace test {

std::vector<CovenantViolation> CovenantSemanticOracle::check(const ExecutionTrace& trace) {
    reset();
    return observeTrace(trace);
}

void CovenantSemanticOracle::reset() {
    // Default: no state to reset
}

// ============================================================================
// Helper Methods
// ============================================================================

bool CovenantSemanticOracle::hasCovenantOps(const ExecutionTrace& trace) const {
    // Check for covenant-related operations
    // For now, basic check for introspection ops
    return !getIntrospectionOps(trace).empty();
}

std::vector<Operation> CovenantSemanticOracle::getIntrospectionOps(const ExecutionTrace& trace) const {
    std::vector<Operation> introspection_ops;

    for (const auto& op : trace.operations) {
        // Covenant introspection operations
        if (op.type == OperationType::OP_COVENANT_CHECK ||
            op.type == OperationType::OUTPUT_SHAPE_CHECK ||
            op.type == OperationType::STATE_TRANSITION_VERIFY ||
            op.type == OperationType::TIME_LOCK_VERIFY) {
            introspection_ops.push_back(op);
        }
    }

    return introspection_ops;
}

bool CovenantSemanticOracle::hasRecursiveCovenant(const ExecutionTrace& trace) const {
    // Check if trace shows recursive covenant pattern
    // Recursive covenants reference themselves or create cycles

    // For Phase 7d: Basic check - look for repeated patterns
    // Full implementation would track output script analysis

    return getMaxRecursionDepth(trace) > 1;
}

size_t CovenantSemanticOracle::countCovenantOps(const ExecutionTrace& trace) const {
    size_t count = 0;

    count += countOperations(trace, OperationType::OP_COVENANT_CHECK);
    count += countOperations(trace, OperationType::OUTPUT_SHAPE_CHECK);
    count += countOperations(trace, OperationType::STATE_TRANSITION_VERIFY);
    count += countOperations(trace, OperationType::TIME_LOCK_VERIFY);

    return count;
}

std::vector<Operation> CovenantSemanticOracle::getOperationsByType(
    const ExecutionTrace& trace, OperationType type) const {

    std::vector<Operation> ops;

    for (const auto& op : trace.operations) {
        if (op.type == type) {
            ops.push_back(op);
        }
    }

    return ops;
}

std::optional<Operation> CovenantSemanticOracle::getOperationAtStep(
    const ExecutionTrace& trace, uint64_t step) const {

    for (const auto& op : trace.operations) {
        if (op.step == step) {
            return op;
        }
    }

    return std::nullopt;
}

bool CovenantSemanticOracle::wasSuccessful(const ExecutionTrace& trace) const {
    return trace.success;
}

size_t CovenantSemanticOracle::getStackSizeAtStep(const ExecutionTrace& trace, uint64_t step) const {
    // For Phase 7d: Simplified stack tracking
    // Full implementation would track stack state at each step

    size_t stack_size = 0;

    for (const auto& op : trace.operations) {
        if (op.step > step) {
            break;
        }

        // Simple heuristic: operations generally consume/produce stack items
        // Real implementation would track actual stack changes
        if (op.success) {
            stack_size++;
        }
    }

    return stack_size;
}

size_t CovenantSemanticOracle::countOperations(const ExecutionTrace& trace, OperationType type) const {
    size_t count = 0;

    for (const auto& op : trace.operations) {
        if (op.type == type) {
            count++;
        }
    }

    return count;
}

bool CovenantSemanticOracle::hasStateTransitions(const ExecutionTrace& trace) const {
    // Check if trace shows state machine transitions
    // For Phase 7d: Look for patterns indicating state changes

    // State transitions typically involve:
    // 1. Reading previous state
    // 2. Validating transition
    // 3. Encoding new state

    return countCovenantOps(trace) > 0 && trace.success;
}

size_t CovenantSemanticOracle::getMaxRecursionDepth(const ExecutionTrace& trace) const {
    // For Phase 7d: Simplified recursion tracking
    // Full implementation would analyze script tree depth

    // Basic heuristic: count nested introspection operations
    size_t max_depth = 0;
    size_t current_depth = 0;

    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_COVENANT_CHECK ||
            op.type == OperationType::OUTPUT_SHAPE_CHECK ||
            op.type == OperationType::STATE_TRANSITION_VERIFY ||
            op.type == OperationType::TIME_LOCK_VERIFY) {
            current_depth++;
            max_depth = std::max(max_depth, current_depth);
        }
    }

    return max_depth;
}

} // namespace test
} // namespace execution
} // namespace dinero
