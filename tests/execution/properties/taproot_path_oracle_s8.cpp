#include "taproot_path_oracle_s8.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<TaprootViolation> S8Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<TaprootViolation> violations;

    // Only check if Taproot path is present
    if (!hasTaprootPath(trace)) {
        return violations;  // No Taproot, property trivially holds
    }

    // Check for leakage
    auto leakage_error = verifyNoLeakage(trace);
    if (leakage_error) {
        TaprootViolation violation(
            getName(),
            "Semantic leakage detected: " + *leakage_error,
            0
        );
        violation.details = "Unrevealed path may have affected execution";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S8Oracle::verifyNoLeakage(const ExecutionTrace& trace) const {
    // Phase 7c: Basic leakage checks
    // Full leakage testing will require comparing multiple executions

    // Check 1: Execution should only depend on revealed paths
    // For key path: no script operations should execute
    if (isKeyPath(trace)) {
        if (hasOperation(trace, OperationType::LEAF_REVEAL) ||
            hasOperation(trace, OperationType::MERKLE_VERIFY)) {
            return "Key path execution shows script path operations";
        }
    }

    // Check 2: For script path: only revealed leaves should be referenced
    if (isScriptPath(trace)) {
        // Verify that MERKLE_VERIFY operations match path reveals
        size_t merkle_ops = countOperations(trace, OperationType::MERKLE_VERIFY);
        size_t reveals = getPathRevealCount(trace);

        // Should have Merkle proof for each reveal
        if (merkle_ops > 0 && merkle_ops != reveals) {
            return "MERKLE_VERIFY count (" + std::to_string(merkle_ops) +
                   ") doesn't match reveals (" + std::to_string(reveals) + ")";
        }
    }

    return std::nullopt;  // No leakage detected
}

} // namespace test
} // namespace execution
} // namespace dinero
