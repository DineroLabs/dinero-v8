#include "consensus_liveness_oracle_dl2.h"
#include <sstream>
#include <set>

namespace dinero {
namespace consensus {
namespace test {

std::vector<LivenessViolation> DL2Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<LivenessViolation> violations;

    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.size() < 2) {
        // Need at least 2 nodes to check propagation
        return violations;
    }

    // Get all unique blocks that were accepted by any honest node
    std::set<std::string> all_blocks;
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.success &&
            event.block_hash.has_value() &&
            !event.block_hash->empty()) {
            all_blocks.insert(*event.block_hash);
        }
    }

    // Check propagation for each block
    for (const auto& block_hash : all_blocks) {
        auto broadcast_time = getBlockBroadcastTime(trace, block_hash);

        if (!broadcast_time) {
            // Block wasn't broadcast in trace (genesis or unknown)
            continue;
        }

        uint64_t deadline = *broadcast_time + propagation_timeout_;

        if (trace.end_time < deadline) {
            // Trace didn't run long enough to verify propagation
            continue;
        }

        // Find nodes that didn't receive block by deadline
        auto missing_nodes = findNodesWithoutBlock(trace, block_hash, deadline, honest_nodes);

        if (!missing_nodes.empty()) {
            // Violation: block didn't propagate to all nodes
            std::ostringstream desc;
            desc << "Block " << block_hash << " broadcast at T=" << *broadcast_time
                 << " failed to reach " << missing_nodes.size() << " node(s) by deadline T="
                 << deadline << " (timeout=" << propagation_timeout_ << "ms)";

            LivenessViolation v(getName(), desc.str(), deadline, trace.end_time);
            v.involved_nodes = missing_nodes;
            v.details = "Missing on: " + (missing_nodes.empty() ? "none" : missing_nodes[0]);
            violations.push_back(v);
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::optional<uint64_t> DL2Oracle::getBlockBroadcastTime(
    const ConsensusTrace& trace,
    const std::string& block_hash
) const {
    // Look for first BLOCK_ACCEPTED event for this block
    std::optional<uint64_t> earliest_time;

    for (const auto& event : trace.events) {
        if (event.block_hash == block_hash &&
            event.type == ConsensusEventType::BLOCK_ACCEPTED) {
            if (!earliest_time || event.timestamp < *earliest_time) {
                earliest_time = event.timestamp;
            }
        }
    }

    return earliest_time;
}

std::optional<uint64_t> DL2Oracle::getBlockReceivedTime(
    const ConsensusTrace& trace,
    const NodeID& node_id,
    const std::string& block_hash
) const {
    // Look for BLOCK_RECEIVED or BLOCK_ACCEPTED event
    for (const auto& event : trace.events) {
        if (event.node_id == node_id && event.block_hash == block_hash) {
            if (event.type == ConsensusEventType::BLOCK_RECEIVED ||
                event.type == ConsensusEventType::BLOCK_ACCEPTED) {
                return event.timestamp;
            }
        }
    }

    return std::nullopt;
}

std::vector<NodeID> DL2Oracle::findNodesWithoutBlock(
    const ConsensusTrace& trace,
    const std::string& block_hash,
    uint64_t deadline,
    const std::vector<NodeID>& honest_nodes
) const {
    std::vector<NodeID> missing_nodes;

    for (const auto& node_id : honest_nodes) {
        auto received_time = getBlockReceivedTime(trace, node_id, block_hash);

        if (!received_time || *received_time > deadline) {
            // Node didn't receive block by deadline
            missing_nodes.push_back(node_id);
        }
    }

    return missing_nodes;
}

} // namespace test
} // namespace consensus
} // namespace dinero
