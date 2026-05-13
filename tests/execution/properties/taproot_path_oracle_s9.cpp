#include "taproot_path_oracle_s9.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<TaprootViolation> S9Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<TaprootViolation> violations;

    // Only check if Taproot script path is present
    if (!hasTaprootPath(trace) || isKeyPath(trace)) {
        return violations;  // Key path or no Taproot, property trivially holds
    }

    // Check commitment completeness
    auto completeness_error = verifyCommitmentCompleteness(trace);
    if (completeness_error) {
        TaprootViolation violation(
            getName(),
            "Commitment incomplete: " + *completeness_error,
            0
        );
        violation.details = "Path commitment structure is invalid";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S9Oracle::verifyCommitmentCompleteness(const ExecutionTrace& trace) const {
    // Phase 7c: Basic commitment checks
    // Full commitment verification will be added with complete Taproot implementation

    // Check 1: Script path should have revealed leaf
    if (isScriptPath(trace)) {
        if (!trace.taproot_path->revealed_leaf) {
            return "Script path has no revealed leaf";
        }
    }

    // Check 2: Each path reveal should have corresponding operations
    for (const auto& reveal : getPathReveals(trace)) {
        // Should have LEAF_REVEAL at this step
        bool has_leaf_reveal = false;
        for (const auto& op : trace.operations) {
            if (op.type == OperationType::LEAF_REVEAL && op.step == reveal.step) {
                has_leaf_reveal = true;
                break;
            }
        }

        if (!has_leaf_reveal) {
            return "Path reveal at step " + std::to_string(reveal.step) +
                   " missing LEAF_REVEAL operation";
        }
    }

    // Check 3: MERKLE_VERIFY should be present for script path reveals
    if (getPathRevealCount(trace) > 0) {
        size_t merkle_count = countOperations(trace, OperationType::MERKLE_VERIFY);
        if (merkle_count == 0) {
            return "Script path reveals without MERKLE_VERIFY operations";
        }
    }

    return std::nullopt;  // Commitment complete
}

} // namespace test
} // namespace execution
} // namespace dinero
