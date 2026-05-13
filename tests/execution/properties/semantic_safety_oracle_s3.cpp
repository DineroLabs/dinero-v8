#include "semantic_safety_oracle_s3.h"
#include <set>

namespace dinero {
namespace execution {
namespace test {

std::vector<SemanticViolation> S3Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<SemanticViolation> violations;

    // Only check if Taproot path is present
    if (!trace.taproot_path) {
        return violations;  // No Taproot, property trivially holds
    }

    // Check 1: Taproot structure is valid
    auto structure_error = verifyTaprootStructure(trace);
    if (structure_error) {
        SemanticViolation violation(
            getName(),
            "Taproot structure invalid: " + *structure_error,
            0
        );
        violation.details = "Taproot path present but structure inconsistent";
        violations.push_back(violation);
    }

    // Check 2: Path isolation is maintained
    auto isolation_error = verifyPathIsolation(trace);
    if (isolation_error) {
        SemanticViolation violation(
            getName(),
            "Path isolation violated: " + *isolation_error,
            0
        );
        violation.details = "Unrevealed leaf may have been activated";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S3Oracle::verifyTaprootStructure(const ExecutionTrace& trace) const {
    // Phase 7b: Basic structure checks
    // Full Taproot validation will be added in Phase 7c

    if (!trace.taproot_path) {
        return std::nullopt;  // No Taproot path
    }

    // Check that path reveals are recorded if Taproot is used
    if (trace.taproot_path->is_key_path) {
        // Key path execution - no reveals expected
        if (!trace.path_reveals.empty()) {
            return "Key path execution has unexpected path reveals";
        }
    } else {
        // Script path execution - should have at least one reveal
        if (trace.path_reveals.empty()) {
            return "Script path execution but no path reveals recorded";
        }
    }

    return std::nullopt;  // Structure valid
}

std::optional<std::string> S3Oracle::verifyPathIsolation(const ExecutionTrace& trace) const {
    // Phase 7b: Basic isolation checks
    // Full isolation testing will be added in Phase 7c

    if (!trace.taproot_path) {
        return std::nullopt;  // No Taproot
    }

    // Check 1: If key path, no script operations should be executed
    if (trace.taproot_path->is_key_path) {
        // Key path should not execute script opcodes
        for (const auto& op : trace.operations) {
            if (op.type != OperationType::OP_PUSH &&
                op.type != OperationType::PATH_SELECT &&
                op.type != OperationType::KEY_PATH_EXECUTE) {
                return "Key path execution contains script operations";
            }
        }
    }

    // Check 2: Path reveals should be unique
    std::set<uint64_t> revealed_steps;
    for (const auto& reveal : trace.path_reveals) {
        if (revealed_steps.count(reveal.step) > 0) {
            return "Duplicate path reveal at step " + std::to_string(reveal.step);
        }
        revealed_steps.insert(reveal.step);
    }

    return std::nullopt;  // Isolation maintained
}

} // namespace test
} // namespace execution
} // namespace dinero
