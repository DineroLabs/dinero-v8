#include "taproot_path_oracle_s7.h"
#include <set>

namespace dinero {
namespace execution {
namespace test {

std::vector<TaprootViolation> S7Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<TaprootViolation> violations;

    // Only check if Taproot script path is present
    if (!hasTaprootPath(trace) || isKeyPath(trace)) {
        return violations;  // No script path, property trivially holds
    }

    // Check reveal independence
    auto independence_error = verifyRevealIndependence(trace);
    if (independence_error) {
        TaprootViolation violation(
            getName(),
            "Reveal independence violated: " + *independence_error,
            0
        );
        violation.details = "Path reveals are not properly isolated";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S7Oracle::verifyRevealIndependence(const ExecutionTrace& trace) const {
    // Phase 7c: Basic independence checks
    // Full partial reveal testing will be added with multi-path scenarios

    // Check 1: All reveals should have unique leaf indices
    std::set<uint32_t> revealed_leaves;
    for (const auto& reveal : getPathReveals(trace)) {
        if (reveal.leaf_index) {
            if (revealed_leaves.count(*reveal.leaf_index) > 0) {
                return "Duplicate leaf reveal at index " + std::to_string(*reveal.leaf_index);
            }
            revealed_leaves.insert(*reveal.leaf_index);
        }
    }

    // Check 2: Each reveal should correspond to exactly one execution
    // (no reveal should trigger multiple executions)
    size_t leaf_reveal_ops = countOperations(trace, OperationType::LEAF_REVEAL);
    if (leaf_reveal_ops != getPathRevealCount(trace)) {
        return "Leaf reveal operation count mismatch";
    }

    return std::nullopt;  // Reveals are independent
}

} // namespace test
} // namespace execution
} // namespace dinero
