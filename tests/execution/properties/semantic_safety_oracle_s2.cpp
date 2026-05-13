#include "semantic_safety_oracle_s2.h"

namespace dinero {
namespace execution {
namespace test {

std::vector<SemanticViolation> S2Oracle::observeTrace(const ExecutionTrace& trace) {
    std::vector<SemanticViolation> violations;

    // Only check if witness is non-empty
    if (trace.witness.elements.empty()) {
        return violations;  // No witness, property trivially holds
    }

    // Check 1: Witness elements are pushed to stack
    auto push_error = verifyWitnessPushed(trace);
    if (push_error) {
        SemanticViolation violation(
            getName(),
            "Witness not pushed to stack: " + *push_error,
            0
        );
        violation.details = "Witness has " +
                           std::to_string(trace.witness.elements.size()) +
                           " elements but not pushed";
        violations.push_back(violation);
    }

    // Check 2: Witness data is used in operations
    auto usage_error = verifyWitnessInOperations(trace);
    if (usage_error) {
        SemanticViolation violation(
            getName(),
            "Witness not used in operations: " + *usage_error,
            0
        );
        violation.details = "Witness provided but execution appears witness-independent";
        violations.push_back(violation);
    }

    // Check 3: Overall witness utilization
    auto util_error = verifyWitnessUtilization(trace);
    if (util_error) {
        SemanticViolation violation(
            getName(),
            "Witness utilization issue: " + *util_error,
            0
        );
        violation.details = "Witness provided but may not affect execution outcome";
        violations.push_back(violation);
    }

    return violations;
}

std::optional<std::string> S2Oracle::verifyWitnessPushed(const ExecutionTrace& trace) const {
    // Count OP_PUSH operations at the beginning of execution
    size_t push_count = 0;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_PUSH) {
            push_count++;
        } else {
            break;  // Stop at first non-push operation
        }
    }

    // Witness elements should be pushed to stack
    if (push_count < trace.witness.elements.size()) {
        return "Expected " + std::to_string(trace.witness.elements.size()) +
               " witness pushes, found " + std::to_string(push_count);
    }

    return std::nullopt;  // Witness pushed
}

std::optional<std::string> S2Oracle::verifyWitnessInOperations(const ExecutionTrace& trace) const {
    // If witness exists, there should be operations beyond just pushing it
    size_t witness_push_count = trace.witness.elements.size();

    if (trace.operations.size() == witness_push_count) {
        return "Only witness push operations, no actual script execution";
    }

    // Check if any operations beyond witness pushes exist
    if (trace.operations.size() > witness_push_count) {
        // At least some operations use the witness
        return std::nullopt;
    }

    return "No operations found beyond witness pushes";
}

std::optional<std::string> S2Oracle::verifyWitnessUtilization(const ExecutionTrace& trace) const {
    // Check if stack operations are performed
    bool has_stack_ops = false;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_DUP ||
            op.type == OperationType::OP_SWAP ||
            op.type == OperationType::OP_DROP ||
            op.type == OperationType::OP_ADD ||
            op.type == OperationType::OP_SUB) {
            has_stack_ops = true;
            break;
        }
    }

    // If witness provided but no stack operations, witness may be unused
    if (!has_stack_ops && !trace.witness.elements.empty()) {
        return "Witness provided but no stack operations performed";
    }

    return std::nullopt;  // Witness appears to be utilized
}

} // namespace test
} // namespace execution
} // namespace dinero
