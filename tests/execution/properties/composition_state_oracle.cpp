#include "composition_state_oracle.h"
#include <algorithm>

namespace dinero {
namespace execution {
namespace test {

std::vector<CompositionViolation> CompositionStateOracle::check(const ExecutionTrace& trace) {
    reset();
    return observeTrace(trace);
}

void CompositionStateOracle::reset() {
    // Default: no state to reset
}

// ============================================================================
// Helper Methods
// ============================================================================

bool CompositionStateOracle::hasMultipleInputs(const ExecutionTrace& trace) const {
    // Check if trace has multiple input markers
    // For Phase 7e: Look for MULTI_INPUT_START events

    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::MULTI_INPUT_START) {
            return true;
        }
    }

    return false;
}

size_t CompositionStateOracle::getInputCount(const ExecutionTrace& trace) const {
    // Count number of inputs in trace
    // For Phase 7e: Count MULTI_INPUT_START events

    size_t count = 0;
    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::MULTI_INPUT_START) {
            count++;
        }
    }

    // If no multi-input markers, assume single input
    return count > 0 ? count : 1;
}

bool CompositionStateOracle::hasParallelExecution(const ExecutionTrace& trace) const {
    // Check if trace shows parallel execution
    // For Phase 7e: Look for concurrent operations (same step number)

    if (trace.operations.size() < 2) {
        return false;
    }

    for (size_t i = 1; i < trace.operations.size(); i++) {
        if (trace.operations[i].step == trace.operations[i-1].step) {
            return true;  // Same step = concurrent
        }
    }

    return false;
}

bool CompositionStateOracle::hasStateUpdates(const ExecutionTrace& trace) const {
    // Check if trace has state update events

    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::STATE_UPDATED) {
            return true;
        }
    }

    return false;
}

std::vector<Operation> CompositionStateOracle::getOperationsByType(
    const ExecutionTrace& trace, OperationType type) const {

    std::vector<Operation> ops;

    for (const auto& op : trace.operations) {
        if (op.type == type) {
            ops.push_back(op);
        }
    }

    return ops;
}

std::optional<Operation> CompositionStateOracle::getOperationAtStep(
    const ExecutionTrace& trace, uint64_t step) const {

    for (const auto& op : trace.operations) {
        if (op.step == step) {
            return op;
        }
    }

    return std::nullopt;
}

bool CompositionStateOracle::wasSuccessful(const ExecutionTrace& trace) const {
    return trace.success;
}

size_t CompositionStateOracle::countOperations(const ExecutionTrace& trace, OperationType type) const {
    size_t count = 0;

    for (const auto& op : trace.operations) {
        if (op.type == type) {
            count++;
        }
    }

    return count;
}

bool CompositionStateOracle::areOperationsIsolated(const ExecutionTrace& trace) const {
    // Check if operations are isolated (don't interfere)
    // For Phase 7e: Look for INPUT_ISOLATED events

    for (const auto& event : trace.events) {
        if (event.type == ExecutionEventType::INPUT_ISOLATED) {
            return true;
        }
    }

    // Default: assume isolated if no multi-input scenario
    return !hasMultipleInputs(trace);
}

size_t CompositionStateOracle::getMaxConcurrentOps(const ExecutionTrace& trace) const {
    // Count maximum concurrent operations
    // For Phase 7e: Count ops with same step number

    if (trace.operations.empty()) {
        return 0;
    }

    size_t max_concurrent = 1;
    size_t current_concurrent = 1;
    uint64_t current_step = trace.operations[0].step;

    for (size_t i = 1; i < trace.operations.size(); i++) {
        if (trace.operations[i].step == current_step) {
            current_concurrent++;
            max_concurrent = std::max(max_concurrent, current_concurrent);
        } else {
            current_step = trace.operations[i].step;
            current_concurrent = 1;
        }
    }

    return max_concurrent;
}

bool CompositionStateOracle::isDeterministic(const ExecutionTrace& trace) const {
    // Check if trace is deterministic
    // For Phase 7e: Verify trace hash is computed

    return trace.final_hash != 0;
}

} // namespace test
} // namespace execution
} // namespace dinero
