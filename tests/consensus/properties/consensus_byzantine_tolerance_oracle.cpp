#include "consensus_byzantine_tolerance_oracle.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

// ============================================================================
// Helper Methods
// ============================================================================

std::vector<NodeID> ByzantineToleranceOracle::getHonestNodes(const ConsensusTrace& trace) const {
    std::vector<NodeID> honest_nodes;

    // Get all nodes
    auto all_nodes = trace.getAllNodes();

    // Filter to only honest nodes (is_byzantine=false)
    for (const auto& node_id : all_nodes) {
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

std::vector<NodeID> ByzantineToleranceOracle::getByzantineNodes(const ConsensusTrace& trace) const {
    std::vector<NodeID> byzantine_nodes;

    // Get all nodes
    auto all_nodes = trace.getAllNodes();

    // Filter to only Byzantine nodes (is_byzantine=true)
    for (const auto& node_id : all_nodes) {
        for (const auto& snapshot : trace.snapshots) {
            if (snapshot.node_id == node_id && snapshot.is_byzantine) {
                byzantine_nodes.push_back(node_id);
                break;
            }
        }
    }

    return byzantine_nodes;
}

std::optional<ConsensusState> ByzantineToleranceOracle::getFinalState(
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

bool ByzantineToleranceOracle::haveHonestNodesConverged(const ConsensusTrace& trace) const {
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
            if (final_state->chain_tip_hash != reference_tip) {
                return false;  // Different tip
            }
            if (final_state->chain_height != reference_height) {
                return false;  // Different height
            }
        }
    }

    return true;  // All honest nodes have same tip
}

std::vector<ConsensusEvent> ByzantineToleranceOracle::getEventsOfType(
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

std::vector<ConsensusEvent> ByzantineToleranceOracle::getEventsForNode(
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

bool ByzantineToleranceOracle::didNetworkMakeProgress(
    const ConsensusTrace& trace,
    uint64_t start_time,
    uint64_t end_time
) const {
    // Check if any BLOCK_ACCEPTED events occurred
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.timestamp >= start_time &&
            event.timestamp <= end_time &&
            event.success) {
            return true;
        }
    }

    return false;
}

} // namespace test
} // namespace consensus
} // namespace dinero
