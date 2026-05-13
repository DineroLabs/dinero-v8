#include "consensus_determinism_oracle_dd5.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<DeterminismViolation> DD5Oracle::observeTraces(
    const std::vector<ConsensusTrace>& traces
) {
    std::vector<DeterminismViolation> violations;

    if (traces.empty() || traces.size() < 2) {
        return violations;
    }

    // Get Byzantine actions from all traces
    auto reference_actions = getByzantineActions(traces[0]);

    // If no Byzantine actions, property trivially holds
    if (reference_actions.empty()) {
        return violations;
    }

    for (size_t i = 1; i < traces.size(); ++i) {
        auto current_actions = getByzantineActions(traces[i]);

        if (!byzantineSequencesEqual(reference_actions, current_actions)) {
            // Violation: Byzantine action sequences differ
            std::ostringstream desc;
            desc << "Byzantine behavior non-determinism: run 0 had "
                 << reference_actions.size() << " Byzantine actions, run " << i
                 << " had " << current_actions.size() << " Byzantine actions.";

            DeterminismViolation v(
                getName(),
                desc.str(),
                traces[0].rng_seed
            );

            std::ostringstream details;
            details << "Byzantine action sequences differ between runs "
                   << "(ref_count=" << reference_actions.size()
                   << ", current_count=" << current_actions.size() << ")";
            v.details = details.str();

            violations.push_back(v);
            break;  // Only report first mismatch
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::vector<ConsensusAction> DD5Oracle::getByzantineActions(
    const ConsensusTrace& trace
) const {
    std::vector<ConsensusAction> byzantine_actions;

    for (const auto& action : trace.actions) {
        // Check if this is a Byzantine-related action
        if (action.type == ConsensusActionType::ENABLE_BYZANTINE ||
            action.type == ConsensusActionType::DISABLE_BYZANTINE ||
            action.type == ConsensusActionType::WITHHOLD_BLOCK ||
            action.type == ConsensusActionType::DOUBLE_SPEND_ATTEMPT) {
            byzantine_actions.push_back(action);
        }
    }

    return byzantine_actions;
}

bool DD5Oracle::byzantineSequencesEqual(
    const std::vector<ConsensusAction>& seq1,
    const std::vector<ConsensusAction>& seq2
) const {
    if (seq1.size() != seq2.size()) {
        return false;
    }

    for (size_t i = 0; i < seq1.size(); ++i) {
        if (!actionsEqual(seq1[i], seq2[i])) {
            return false;
        }
    }

    return true;
}

bool DD5Oracle::actionsEqual(
    const ConsensusAction& a1,
    const ConsensusAction& a2
) const {
    // Compare critical action fields for determinism
    return a1.type == a2.type &&
           a1.timestamp == a2.timestamp &&
           a1.sequence_number == a2.sequence_number &&
           a1.node_id == a2.node_id;
}

} // namespace test
} // namespace consensus
} // namespace dinero
