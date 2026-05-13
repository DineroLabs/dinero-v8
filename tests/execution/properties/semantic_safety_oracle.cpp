#include "semantic_safety_oracle.h"
#include <set>

namespace dinero {
namespace execution {
namespace test {

std::vector<Operation> SemanticSafetyOracle::getOperationsByType(
    const ExecutionTrace& trace,
    OperationType type
) const {
    std::vector<Operation> filtered;
    for (const auto& op : trace.operations) {
        if (op.type == type) {
            filtered.push_back(op);
        }
    }
    return filtered;
}

std::optional<Operation> SemanticSafetyOracle::getOperationAtStep(
    const ExecutionTrace& trace,
    uint64_t step
) const {
    for (const auto& op : trace.operations) {
        if (op.step == step) {
            return op;
        }
    }
    return std::nullopt;
}

std::vector<StackSnapshot> SemanticSafetyOracle::getStackSnapshots(
    const ExecutionTrace& trace
) const {
    return trace.stack_states;
}

std::optional<StackSnapshot> SemanticSafetyOracle::getStackAtStep(
    const ExecutionTrace& trace,
    uint64_t step
) const {
    for (const auto& snapshot : trace.stack_states) {
        if (snapshot.step == step) {
            return snapshot;
        }
    }
    return std::nullopt;
}

bool SemanticSafetyOracle::wasSuccessful(const ExecutionTrace& trace) const {
    return trace.success;
}

bool SemanticSafetyOracle::hasFailed(const ExecutionTrace& trace) const {
    return !trace.success;
}

std::optional<std::string> SemanticSafetyOracle::getError(const ExecutionTrace& trace) const {
    return trace.error;
}

uint64_t SemanticSafetyOracle::getOperationCount(const ExecutionTrace& trace) const {
    return trace.operation_count;
}

uint64_t SemanticSafetyOracle::getMaxStackDepth(const ExecutionTrace& trace) const {
    return trace.stack_depth_max;
}

} // namespace test
} // namespace execution
} // namespace dinero
