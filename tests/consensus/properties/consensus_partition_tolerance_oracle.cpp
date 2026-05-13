#include "consensus_partition_tolerance_oracle.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

// ============================================================================
// Helper Methods
// ============================================================================

std::vector<NodeID> PartitionToleranceOracle::getHonestNodes(const ConsensusTrace& trace) const {
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

std::optional<ConsensusState> PartitionToleranceOracle::getFinalState(
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

std::vector<ConsensusEvent> PartitionToleranceOracle::getEventsOfType(
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

std::optional<uint64_t> PartitionToleranceOracle::getPartitionStartTime(const ConsensusTrace& trace) const {
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

std::optional<uint64_t> PartitionToleranceOracle::getPartitionHealTime(const ConsensusTrace& trace) const {
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

std::vector<std::vector<NodeID>> PartitionToleranceOracle::getPartitionGroups(
    const ConsensusTrace& trace
) const {
    // For Phase 5d simplified implementation:
    // Look for PARTITION_NETWORK actions with node_group data
    std::vector<std::vector<NodeID>> groups;

    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::PARTITION_NETWORK &&
            action.node_group.has_value() &&
            !action.node_group->empty()) {
            groups.push_back(*action.node_group);
        }
    }

    return groups;
}

bool PartitionToleranceOracle::didNodeMakeProgress(
    const ConsensusTrace& trace,
    const NodeID& node_id,
    uint64_t start_time,
    uint64_t end_time
) const {
    std::optional<uint32_t> start_height;
    std::optional<uint32_t> end_height;

    // Find node's height at start_time and end_time
    for (const auto& snapshot : trace.snapshots) {
        if (snapshot.node_id == node_id) {
            if (snapshot.timestamp >= start_time && !start_height) {
                start_height = snapshot.chain_height;
            }
            if (snapshot.timestamp >= end_time) {
                end_height = snapshot.chain_height;
                break;
            }
        }
    }

    // Check events for height changes
    for (const auto& event : trace.events) {
        if (event.node_id == node_id &&
            event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_height.has_value()) {

            if (event.timestamp >= start_time && event.timestamp <= end_time) {
                if (!start_height) start_height = *event.block_height;
                end_height = *event.block_height;
            }
        }
    }

    if (start_height && end_height) {
        return *end_height > *start_height;
    }

    return false;
}

bool PartitionToleranceOracle::haveNodesConverged(
    const ConsensusTrace& trace,
    const std::vector<NodeID>& nodes
) const {
    if (nodes.size() < 2) {
        return true;  // Trivially converged with 0-1 nodes
    }

    // Check if all nodes have the same chain tip
    std::string reference_tip;
    uint32_t reference_height = 0;

    for (const auto& node_id : nodes) {
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

uint32_t PartitionToleranceOracle::getMaxHeightAtTime(
    const ConsensusTrace& trace,
    const std::vector<NodeID>& nodes,
    uint64_t timestamp
) const {
    uint32_t max_height = 0;

    // Check snapshots
    for (const auto& snapshot : trace.snapshots) {
        if (snapshot.timestamp <= timestamp) {
            auto it = std::find(nodes.begin(), nodes.end(), snapshot.node_id);
            if (it != nodes.end() && snapshot.chain_height > max_height) {
                max_height = snapshot.chain_height;
            }
        }
    }

    // Check events
    for (const auto& event : trace.events) {
        if (event.timestamp <= timestamp &&
            event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_height.has_value()) {

            auto it = std::find(nodes.begin(), nodes.end(), event.node_id);
            if (it != nodes.end() && *event.block_height > max_height) {
                max_height = *event.block_height;
            }
        }
    }

    return max_height;
}

std::optional<std::vector<NodeID>> PartitionToleranceOracle::getMajorityPartition(
    const ConsensusTrace& trace,
    const std::vector<std::vector<NodeID>>& partitions
) const {
    auto honest_nodes = getHonestNodes(trace);
    size_t total_honest = honest_nodes.size();
    size_t majority_threshold = (total_honest / 2) + 1;

    for (const auto& partition : partitions) {
        // Count honest nodes in this partition
        size_t honest_count = 0;
        for (const auto& node_id : partition) {
            auto it = std::find(honest_nodes.begin(), honest_nodes.end(), node_id);
            if (it != honest_nodes.end()) {
                honest_count++;
            }
        }

        if (honest_count >= majority_threshold) {
            return partition;
        }
    }

    return std::nullopt;
}

std::vector<std::vector<NodeID>> PartitionToleranceOracle::getMinorityPartitions(
    const ConsensusTrace& trace,
    const std::vector<std::vector<NodeID>>& partitions
) const {
    std::vector<std::vector<NodeID>> minority_partitions;
    auto honest_nodes = getHonestNodes(trace);
    size_t total_honest = honest_nodes.size();
    size_t majority_threshold = (total_honest / 2) + 1;

    for (const auto& partition : partitions) {
        // Count honest nodes in this partition
        size_t honest_count = 0;
        for (const auto& node_id : partition) {
            auto it = std::find(honest_nodes.begin(), honest_nodes.end(), node_id);
            if (it != honest_nodes.end()) {
                honest_count++;
            }
        }

        if (honest_count < majority_threshold) {
            minority_partitions.push_back(partition);
        }
    }

    return minority_partitions;
}

} // namespace test
} // namespace consensus
} // namespace dinero
