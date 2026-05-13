#include "consensus_liveness_oracle_dl5.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<LivenessViolation> DL5Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<LivenessViolation> violations;

    // For Phase 5c simplified implementation:
    // Check if all nodes converged to similar heights by end of trace
    // Full implementation would track individual node sync progress

    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.size() < 2) {
        // Need at least 2 nodes to check sync
        return violations;
    }

    // Find nodes that started during the trace (NODE_START events)
    std::vector<NodeID> syncing_nodes;
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::NODE_START &&
            action.node_id.has_value()) {
            syncing_nodes.push_back(*action.node_id);
        }
    }

    if (syncing_nodes.empty()) {
        // No nodes started during trace - nothing to check
        return violations;
    }

    // Check each syncing node
    for (const auto& node_id : syncing_nodes) {
        auto start_time = getNodeStartTime(trace, node_id);
        if (!start_time) {
            continue;
        }

        uint64_t deadline = *start_time + sync_timeout_;

        if (trace.end_time < deadline) {
            // Trace didn't run long enough to verify sync
            continue;
        }

        // Get target height (network height when node started)
        uint32_t target_height = getNetworkHeightAtTime(trace, *start_time);

        // Get node's actual height at deadline
        auto actual_height = getNodeHeightAtTime(trace, node_id, deadline);

        if (!actual_height || *actual_height < target_height) {
            // Violation: node didn't reach target height
            std::ostringstream desc;
            desc << "Node " << node_id << " started at T=" << *start_time
                 << " but failed to sync to height " << target_height
                 << " by deadline T=" << deadline << " (timeout=" << sync_timeout_ << "ms). ";

            if (actual_height) {
                desc << "Reached height " << *actual_height << " (behind by "
                     << (target_height - *actual_height) << " blocks)";
            } else {
                desc << "No height data available";
            }

            LivenessViolation v(getName(), desc.str(), deadline, trace.end_time);
            v.involved_nodes = {node_id};
            v.details = "Target: " + std::to_string(target_height) +
                       ", Actual: " + (actual_height ? std::to_string(*actual_height) : "unknown");
            violations.push_back(v);
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::optional<uint64_t> DL5Oracle::getNodeStartTime(
    const ConsensusTrace& trace,
    const NodeID& node_id
) const {
    // Look for NODE_START action for this node
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::NODE_START &&
            action.node_id == node_id) {
            return action.timestamp;
        }
    }

    return std::nullopt;
}

uint32_t DL5Oracle::getNetworkHeightAtTime(
    const ConsensusTrace& trace,
    uint64_t timestamp
) const {
    uint32_t max_height = 0;

    // Find max height across all nodes at or before timestamp
    for (const auto& event : trace.events) {
        if (event.timestamp > timestamp) {
            break;
        }

        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_height.has_value() &&
            *event.block_height > max_height) {
            max_height = *event.block_height;
        }
    }

    return max_height;
}

std::optional<uint32_t> DL5Oracle::getNodeHeightAtTime(
    const ConsensusTrace& trace,
    const NodeID& node_id,
    uint64_t timestamp
) const {
    std::optional<uint32_t> height;

    // Find node's latest height at or before timestamp
    for (const auto& event : trace.events) {
        if (event.timestamp > timestamp) {
            break;
        }

        if (event.node_id == node_id &&
            event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_height.has_value()) {
            height = event.block_height;
        }
    }

    // Also check snapshots
    for (const auto& snapshot : trace.snapshots) {
        if (snapshot.node_id == node_id &&
            snapshot.timestamp <= timestamp) {
            if (!height || snapshot.chain_height > *height) {
                height = snapshot.chain_height;
            }
        }
    }

    return height;
}

} // namespace test
} // namespace consensus
} // namespace dinero
