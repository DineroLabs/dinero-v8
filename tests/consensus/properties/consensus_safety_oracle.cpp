#include "consensus_safety_oracle.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

// ============================================================================
// Helper Methods
// ============================================================================

std::vector<NodeID> ConsensusSafetyOracle::getHonestNodes(const ConsensusTrace& trace) const {
    std::vector<NodeID> honest_nodes;

    // Get all nodes
    auto all_nodes = trace.getAllNodes();

    // Filter out Byzantine nodes by checking final state snapshots
    for (const auto& node_id : all_nodes) {
        // Check if node was Byzantine in any snapshot
        bool is_byzantine = false;
        for (const auto& snapshot : trace.snapshots) {
            if (snapshot.node_id == node_id && snapshot.is_byzantine) {
                is_byzantine = true;
                break;
            }
        }

        if (!is_byzantine) {
            honest_nodes.push_back(node_id);
        }
    }

    return honest_nodes;
}

std::vector<NodeID> ConsensusSafetyOracle::getByzantineNodes(const ConsensusTrace& trace) const {
    std::vector<NodeID> byzantine_nodes;

    // Get all nodes
    auto all_nodes = trace.getAllNodes();

    // Find Byzantine nodes by checking snapshots
    for (const auto& node_id : all_nodes) {
        // Check if node was Byzantine in any snapshot
        for (const auto& snapshot : trace.snapshots) {
            if (snapshot.node_id == node_id && snapshot.is_byzantine) {
                byzantine_nodes.push_back(node_id);
                break;  // Found it, move to next node
            }
        }
    }

    return byzantine_nodes;
}

std::optional<ConsensusState> ConsensusSafetyOracle::getFinalState(
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

std::vector<ConsensusEvent> ConsensusSafetyOracle::getBlockAcceptedEvents(
    const ConsensusTrace& trace,
    const NodeID& node_id
) const {
    std::vector<ConsensusEvent> accepted_events;

    for (const auto& event : trace.events) {
        if (event.node_id == node_id && event.type == ConsensusEventType::BLOCK_ACCEPTED) {
            accepted_events.push_back(event);
        }
    }

    return accepted_events;
}

std::vector<ConsensusEvent> ConsensusSafetyOracle::getChainTipChangedEvents(
    const ConsensusTrace& trace,
    const NodeID& node_id
) const {
    std::vector<ConsensusEvent> tip_changed_events;

    for (const auto& event : trace.events) {
        if (event.node_id == node_id && event.type == ConsensusEventType::CHAIN_TIP_CHANGED) {
            tip_changed_events.push_back(event);
        }
    }

    return tip_changed_events;
}

} // namespace test
} // namespace consensus
} // namespace dinero
