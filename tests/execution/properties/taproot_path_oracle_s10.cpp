#include "taproot_path_oracle_s10.h"
#include <set>

namespace dinero {
namespace execution {
namespace test {

std::vector<TaprootViolation> S10Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<TaprootViolation> violations;

    // Only check if Taproot script path is present
    if (!hasTaprootPath(trace) || isKeyPath(trace)) {
        return violations;  // Key path or no Taproot, property trivially holds
    }

    // Check leaf uniqueness
    auto uniqueness_error = verifyLeafUniqueness(trace);
    if (uniqueness_error) {
        TaprootViolation violation(
            getName(),
            "Leaf uniqueness violated: " + *uniqueness_error,
            0
        );
        violation.details = "Same leaf executed multiple times";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S10Oracle::verifyLeafUniqueness(const ExecutionTrace& trace) const {
    // Check 1: Each leaf index should appear at most once
    std::set<uint32_t> seen_leaves;

    for (const auto& reveal : getPathReveals(trace)) {
        if (reveal.leaf_index) {
            if (seen_leaves.count(*reveal.leaf_index) > 0) {
                return "Leaf " + std::to_string(*reveal.leaf_index) + " revealed multiple times";
            }
            seen_leaves.insert(*reveal.leaf_index);
        }
    }

    // Check 2: Each LEAF_REVEAL operation should be unique
    std::set<uint64_t> reveal_steps;

    for (const auto& op : trace.operations) {
        if (op.type == OperationType::LEAF_REVEAL) {
            if (reveal_steps.count(op.step) > 0) {
                return "Duplicate LEAF_REVEAL at step " + std::to_string(op.step);
            }
            reveal_steps.insert(op.step);
        }
    }

    // Check 3: Number of unique leaf reveals matches total reveals
    if (seen_leaves.size() != getPathRevealCount(trace)) {
        return "Unique leaf count mismatch (unique=" +
               std::to_string(seen_leaves.size()) +
               ", total=" + std::to_string(getPathRevealCount(trace)) + ")";
    }

    return std::nullopt;  // Each leaf executes at most once
}

} // namespace test
} // namespace execution
} // namespace dinero
