#include "taproot_path_oracle_s6.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<TaprootViolation> S6Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<TaprootViolation> violations;

    // Only check if Taproot path is present
    if (!hasTaprootPath(trace)) {
        return violations;  // No Taproot, property trivially holds
    }

    // Check 1: Key path should have no reveals
    if (isKeyPath(trace)) {
        auto key_path_error = verifyKeyPathNoReveals(trace);
        if (key_path_error) {
            TaprootViolation violation(
                getName(),
                "Key path violation: " + *key_path_error,
                0
            );
            violation.details = "Key path should not reveal any script paths";
            violations.push_back(violation);
        }
    }

    // Check 2: Script path must have reveals
    if (isScriptPath(trace)) {
        auto script_path_error = verifyScriptPathReveals(trace);
        if (script_path_error) {
            TaprootViolation violation(
                getName(),
                "Script path violation: " + *script_path_error,
                0
            );
            violation.details = "Script path must reveal at least one leaf";
            violations.push_back(violation);
        }
    }

    // Check 3: Hidden paths remain inactive
    auto hidden_error = verifyHiddenPathsInactive(trace);
    if (hidden_error) {
        TaprootViolation violation(
            getName(),
            "Hidden path activation: " + *hidden_error,
            0
        );
        violation.details = "Unrevealed path appears to have executed";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S6Oracle::verifyKeyPathNoReveals(const ExecutionTrace& trace) const {
    // Key path should not have any path reveals
    if (getPathRevealCount(trace) > 0) {
        return "Key path has " + std::to_string(getPathRevealCount(trace)) + " path reveals (expected 0)";
    }

    // Key path should not have LEAF_REVEAL operations
    if (hasOperation(trace, OperationType::LEAF_REVEAL)) {
        return "Key path has LEAF_REVEAL operations";
    }

    // Key path should not have MERKLE_VERIFY operations
    if (hasOperation(trace, OperationType::MERKLE_VERIFY)) {
        return "Key path has MERKLE_VERIFY operations";
    }

    return std::nullopt;  // Valid key path
}

std::optional<std::string> S6Oracle::verifyScriptPathReveals(const ExecutionTrace& trace) const {
    // Script path must have at least one path reveal
    if (getPathRevealCount(trace) == 0) {
        return "Script path has no path reveals";
    }

    // Script path should have LEAF_REVEAL operations
    size_t leaf_reveal_count = countOperations(trace, OperationType::LEAF_REVEAL);
    if (leaf_reveal_count == 0) {
        return "Script path has no LEAF_REVEAL operations";
    }

    // Number of reveals should match number of LEAF_REVEAL operations
    if (getPathRevealCount(trace) != leaf_reveal_count) {
        return "Path reveal count (" + std::to_string(getPathRevealCount(trace)) +
               ") doesn't match LEAF_REVEAL operations (" + std::to_string(leaf_reveal_count) + ")";
    }

    return std::nullopt;  // Valid script path
}

std::optional<std::string> S6Oracle::verifyHiddenPathsInactive(const ExecutionTrace& trace) const {
    // For each LEAF_REVEAL operation, verify it has a corresponding path reveal
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::LEAF_REVEAL) {
            // Check if there's a path reveal at this step
            auto reveal = getPathRevealAt(trace, op.step);
            if (!reveal) {
                return "LEAF_REVEAL operation at step " + std::to_string(op.step) +
                       " without corresponding path reveal";
            }
        }
    }

    // Verify all path reveals have corresponding LEAF_REVEAL operations
    for (const auto& reveal : getPathReveals(trace)) {
        bool found = false;
        for (const auto& op : trace.operations) {
            if (op.type == OperationType::LEAF_REVEAL && op.step == reveal.step) {
                found = true;
                break;
            }
        }
        if (!found) {
            return "Path reveal at step " + std::to_string(reveal.step) +
                   " without corresponding LEAF_REVEAL operation";
        }
    }

    return std::nullopt;  // Hidden paths are inactive
}

} // namespace test
} // namespace execution
} // namespace dinero
