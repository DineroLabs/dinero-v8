#include "semantic_determinism_oracle.h"

namespace dinero {
namespace execution {
namespace test {

std::optional<std::string> SemanticDeterminismOracle::compareTraces(
    const ExecutionTrace& trace1,
    const ExecutionTrace& trace2
) const {
    // Check final hash equality (strongest indicator of equivalence)
    if (trace1.final_hash != trace2.final_hash) {
        return "Final hashes differ";
    }

    // Check success/failure outcome
    if (trace1.success != trace2.success) {
        return "Success outcomes differ";
    }

    // Check final state equality
    if (trace1.final_state.stack != trace2.final_state.stack) {
        return "Final stack states differ";
    }

    // Note: ExecutionState doesn't have alt_stack field
    // Only checking main stack is sufficient for Phase 7f

    return std::nullopt;  // Traces are equivalent
}

std::optional<std::string> SemanticDeterminismOracle::verifyHashEquality(
    const std::vector<ExecutionTrace>& traces
) const {
    if (traces.size() < 2) {
        return std::nullopt;  // Nothing to compare
    }

    uint64_t reference_hash = traces[0].final_hash;
    for (size_t i = 1; i < traces.size(); ++i) {
        if (traces[i].final_hash != reference_hash) {
            return "Trace " + std::to_string(i) + " has different hash";
        }
    }

    return std::nullopt;  // All hashes equal
}

std::optional<std::string> SemanticDeterminismOracle::verifyOutcomeEquality(
    const std::vector<ExecutionTrace>& traces
) const {
    if (traces.size() < 2) {
        return std::nullopt;  // Nothing to compare
    }

    bool reference_success = traces[0].success;
    for (size_t i = 1; i < traces.size(); ++i) {
        if (traces[i].success != reference_success) {
            return "Trace " + std::to_string(i) + " has different outcome";
        }
    }

    return std::nullopt;  // All outcomes equal
}

std::optional<std::string> SemanticDeterminismOracle::verifyFinalized(
    const std::vector<ExecutionTrace>& traces
) const {
    for (size_t i = 0; i < traces.size(); ++i) {
        if (traces[i].final_hash == 0) {
            return "Trace " + std::to_string(i) + " not finalized";
        }
    }

    return std::nullopt;  // All finalized
}

} // namespace test
} // namespace execution
} // namespace dinero
