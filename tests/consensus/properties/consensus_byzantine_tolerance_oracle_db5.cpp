#include "consensus_byzantine_tolerance_oracle_db5.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<ByzantineViolation> DB5Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<ByzantineViolation> violations;

    // Find all invalid blocks
    auto invalid_blocks = getInvalidBlocks(trace);
    if (invalid_blocks.empty()) {
        // No invalid blocks detected - property trivially holds
        return violations;
    }

    // Check if any honest nodes accepted invalid blocks
    for (const auto& block_hash : invalid_blocks) {
        if (wasBlockAcceptedByHonestNode(trace, block_hash)) {
            // Violation: Honest node accepted an invalid block
            auto accepting_nodes = getNodesThatAcceptedBlock(trace, block_hash);

            std::ostringstream desc;
            desc << "Honest node(s) accepted invalid block "
                 << block_hash.substr(0, 8) << "... from Byzantine node.";

            ByzantineViolation v(
                getName(),
                desc.str(),
                trace.end_time
            );

            // Report which honest nodes accepted the invalid block
            auto honest_nodes = getHonestNodes(trace);
            for (const auto& node_id : accepting_nodes) {
                // Check if this accepting node is honest
                bool is_honest = false;
                for (const auto& honest_node : honest_nodes) {
                    if (node_id == honest_node) {
                        is_honest = true;
                        break;
                    }
                }
                if (is_honest) {
                    v.involved_nodes.push_back(node_id);
                }
            }

            std::ostringstream details;
            details << "Invalid block " << block_hash
                   << " accepted by " << v.involved_nodes.size() << " honest node(s)";
            v.details = details.str();

            violations.push_back(v);
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::unordered_set<std::string> DB5Oracle::getInvalidBlocks(
    const ConsensusTrace& trace
) const {
    std::unordered_set<std::string> invalid_blocks;

    // Look for BLOCK_REJECTED events indicating invalid blocks
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_REJECTED && !event.success) {
            // Block was rejected - check if due to validation failure
            if (event.error_message.find("invalid") != std::string::npos ||
                event.error_message.find("validation") != std::string::npos) {
                if (event.block_hash && !event.block_hash->empty()) {
                    invalid_blocks.insert(*event.block_hash);
                }
            }
        }
    }

    return invalid_blocks;
}

bool DB5Oracle::wasBlockAcceptedByHonestNode(
    const ConsensusTrace& trace,
    const std::string& block_hash
) const {
    auto honest_nodes = getHonestNodes(trace);

    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_hash && *event.block_hash == block_hash &&
            event.success) {

            // Check if this accepting node is honest
            for (const auto& honest_node : honest_nodes) {
                if (event.node_id == honest_node) {
                    return true;  // Honest node accepted invalid block
                }
            }
        }
    }

    return false;
}

std::vector<NodeID> DB5Oracle::getNodesThatAcceptedBlock(
    const ConsensusTrace& trace,
    const std::string& block_hash
) const {
    std::vector<NodeID> accepting_nodes;

    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_hash && *event.block_hash == block_hash &&
            event.success) {
            accepting_nodes.push_back(event.node_id);
        }
    }

    return accepting_nodes;
}

} // namespace test
} // namespace consensus
} // namespace dinero
