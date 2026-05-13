#include "semantic_safety_oracle_s4.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<SemanticViolation> S4Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<SemanticViolation> violations;

    // Only check if Taproot path is present
    if (!trace.taproot_path) {
        return violations;  // No Taproot, property trivially holds
    }

    // Check path distinction
    auto distinction_error = verifyPathDistinction(trace);
    if (distinction_error) {
        SemanticViolation violation(
            getName(),
            "Path distinction unclear: " + *distinction_error,
            0
        );
        violation.details = "Cannot distinguish execution path from trace";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S4Oracle::verifyPathDistinction(const ExecutionTrace& trace) const {
    // Phase 7b: Basic path distinction checks
    // Full path semantics testing will be added in Phase 7c

    if (!trace.taproot_path) {
        return std::nullopt;  // No Taproot
    }

    // Check 1: Path type is clearly indicated
    bool has_key_path_ops = false;
    bool has_script_path_ops = false;

    for (const auto& op : trace.operations) {
        if (op.type == OperationType::KEY_PATH_EXECUTE) {
            has_key_path_ops = true;
        }
        if (op.type == OperationType::LEAF_REVEAL ||
            op.type == OperationType::MERKLE_VERIFY) {
            has_script_path_ops = true;
        }
    }

    // Check 2: Execution should be either key path OR script path, not both
    if (has_key_path_ops && has_script_path_ops) {
        return "Trace shows both key path and script path operations";
    }

    // Check 3: Key path should not have script operations
    if (trace.taproot_path->is_key_path && has_script_path_ops) {
        return "Key path trace contains script path operations";
    }

    // Check 4: Script path should not have key path operations
    if (!trace.taproot_path->is_key_path && has_key_path_ops) {
        return "Script path trace contains key path operations";
    }

    return std::nullopt;  // Path distinction clear
}

} // namespace test
} // namespace execution
} // namespace dinero
