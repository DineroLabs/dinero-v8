#include "taproot_path_oracle.h"

namespace dinero {
namespace execution {
namespace test {

bool TaprootPathOracle::hasTaprootPath(const ExecutionTrace& trace) const {
    return trace.taproot_path.has_value();
}

bool TaprootPathOracle::isKeyPath(const ExecutionTrace& trace) const {
    return trace.taproot_path && trace.taproot_path->is_key_path;
}

bool TaprootPathOracle::isScriptPath(const ExecutionTrace& trace) const {
    return trace.taproot_path && !trace.taproot_path->is_key_path;
}

size_t TaprootPathOracle::getPathRevealCount(const ExecutionTrace& trace) const {
    return trace.path_reveals.size();
}

std::vector<PathActivation> TaprootPathOracle::getPathReveals(const ExecutionTrace& trace) const {
    return trace.path_reveals;
}

std::optional<PathActivation> TaprootPathOracle::getPathRevealAt(
    const ExecutionTrace& trace,
    uint64_t step
) const {
    for (const auto& reveal : trace.path_reveals) {
        if (reveal.step == step) {
            return reveal;
        }
    }
    return std::nullopt;
}

bool TaprootPathOracle::hasOperation(const ExecutionTrace& trace, OperationType type) const {
    for (const auto& op : trace.operations) {
        if (op.type == type) {
            return true;
        }
    }
    return false;
}

size_t TaprootPathOracle::countOperations(const ExecutionTrace& trace, OperationType type) const {
    size_t count = 0;
    for (const auto& op : trace.operations) {
        if (op.type == type) {
            count++;
        }
    }
    return count;
}

} // namespace test
} // namespace execution
} // namespace dinero
