#include "consensus_determinism_oracle.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

// ============================================================================
// Helper Methods
// ============================================================================

bool ConsensusDeterminismOracle::tracesHaveSameHash(
    const std::vector<ConsensusTrace>& traces
) const {
    if (traces.empty()) {
        return true;
    }

    uint64_t reference_hash = traces[0].final_hash;

    for (size_t i = 1; i < traces.size(); ++i) {
        if (traces[i].final_hash != reference_hash) {
            return false;
        }
    }

    return true;
}

bool ConsensusDeterminismOracle::tracesHaveSameEventCount(
    const std::vector<ConsensusTrace>& traces
) const {
    if (traces.empty()) {
        return true;
    }

    size_t reference_count = traces[0].events.size();

    for (size_t i = 1; i < traces.size(); ++i) {
        if (traces[i].events.size() != reference_count) {
            return false;
        }
    }

    return true;
}

bool ConsensusDeterminismOracle::tracesHaveSameEventSequence(
    const std::vector<ConsensusTrace>& traces
) const {
    if (traces.empty()) {
        return true;
    }

    const auto& reference_events = traces[0].events;

    for (size_t i = 1; i < traces.size(); ++i) {
        const auto& current_events = traces[i].events;

        if (current_events.size() != reference_events.size()) {
            return false;
        }

        for (size_t j = 0; j < reference_events.size(); ++j) {
            if (!eventsEqual(reference_events[j], current_events[j])) {
                return false;
            }
        }
    }

    return true;
}

bool ConsensusDeterminismOracle::tracesHaveSameFinalState(
    const std::vector<ConsensusTrace>& traces
) const {
    if (traces.empty()) {
        return true;
    }

    // Get all nodes from first trace
    auto nodes = traces[0].getAllNodes();

    // Check each node's final state across all traces
    for (const auto& node_id : nodes) {
        auto reference_state = getFinalStateForNode(traces[0], node_id);
        if (!reference_state) {
            continue;  // Node has no state in reference trace
        }

        for (size_t i = 1; i < traces.size(); ++i) {
            auto current_state = getFinalStateForNode(traces[i], node_id);
            if (!current_state) {
                return false;  // Node missing in current trace
            }

            if (!statesEqual(*reference_state, *current_state)) {
                return false;  // States differ
            }
        }
    }

    return true;
}

std::optional<ConsensusState> ConsensusDeterminismOracle::getFinalStateForNode(
    const ConsensusTrace& trace,
    const NodeID& node_id
) const {
    std::optional<ConsensusState> final_state;

    // Find the latest snapshot for this node
    for (const auto& snapshot : trace.snapshots) {
        if (snapshot.node_id == node_id) {
            if (!final_state || snapshot.timestamp > final_state->timestamp) {
                final_state = snapshot;
            }
        }
    }

    return final_state;
}

bool ConsensusDeterminismOracle::eventsEqual(
    const ConsensusEvent& e1,
    const ConsensusEvent& e2
) const {
    // Compare critical event fields for determinism
    return e1.type == e2.type &&
           e1.timestamp == e2.timestamp &&
           e1.sequence_number == e2.sequence_number &&
           e1.node_id == e2.node_id &&
           e1.success == e2.success;
}

bool ConsensusDeterminismOracle::statesEqual(
    const ConsensusState& s1,
    const ConsensusState& s2
) const {
    // Compare critical state fields for determinism
    return s1.chain_tip_hash == s2.chain_tip_hash &&
           s1.chain_height == s2.chain_height &&
           s1.chainwork == s2.chainwork &&
           s1.is_byzantine == s2.is_byzantine;
}

} // namespace test
} // namespace consensus
} // namespace dinero
