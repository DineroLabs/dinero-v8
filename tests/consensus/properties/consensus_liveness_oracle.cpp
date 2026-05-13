#include "consensus_liveness_oracle.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

// ============================================================================
// Helper Methods
// ============================================================================

std::vector<NodeID> ConsensusLivenessOracle::getHonestNodes(const ConsensusTrace& trace) const {
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

std::optional<ConsensusState> ConsensusLivenessOracle::getFinalState(
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

std::vector<ConsensusEvent> ConsensusLivenessOracle::getEventsOfType(
    const ConsensusTrace& trace,
    ConsensusEventType type
) const {
    std::vector<ConsensusEvent> matching_events;

    for (const auto& event : trace.events) {
        if (event.type == type) {
            matching_events.push_back(event);
        }
    }

    return matching_events;
}

std::vector<ConsensusEvent> ConsensusLivenessOracle::getEventsForNode(
    const ConsensusTrace& trace,
    const NodeID& node_id
) const {
    std::vector<ConsensusEvent> node_events;

    for (const auto& event : trace.events) {
        if (event.node_id == node_id) {
            node_events.push_back(event);
        }
    }

    return node_events;
}

std::optional<uint64_t> ConsensusLivenessOracle::getPartitionHealTime(const ConsensusTrace& trace) const {
    // Look for PARTITION_HEALED events
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::PARTITION_HEALED) {
            return event.timestamp;
        }
    }

    // Also check actions
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::HEAL_PARTITION) {
            return action.timestamp;
        }
    }

    return std::nullopt;
}

std::optional<uint64_t> ConsensusLivenessOracle::getPartitionStartTime(const ConsensusTrace& trace) const {
    // Look for PARTITION_ACTIVATED events
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::PARTITION_ACTIVATED) {
            return event.timestamp;
        }
    }

    // Also check actions
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::PARTITION_NETWORK) {
            return action.timestamp;
        }
    }

    return std::nullopt;
}

bool ConsensusLivenessOracle::haveNodesConverged(const ConsensusTrace& trace) const {
    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.size() < 2) {
        return true;  // Trivially converged with 0-1 nodes
    }

    // Check if all honest nodes have the same chain tip
    std::string reference_tip;
    uint32_t reference_height = 0;

    for (const auto& node_id : honest_nodes) {
        auto final_state = getFinalState(trace, node_id);
        if (!final_state) {
            return false;  // Node has no state
        }

        if (reference_tip.empty()) {
            reference_tip = final_state->chain_tip_hash;
            reference_height = final_state->chain_height;
        } else {
            // Check if this node matches the reference
            if (final_state->chain_tip_hash != reference_tip) {
                return false;  // Different tip
            }
            if (final_state->chain_height != reference_height) {
                return false;  // Different height
            }
        }
    }

    return true;  // All nodes have same tip
}

} // namespace test
} // namespace consensus
} // namespace dinero
