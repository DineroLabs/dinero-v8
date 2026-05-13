#include "consensus_safety_oracle_dc1.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<Violation> DC1Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<Violation> violations;

    // Get honest nodes
    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.size() < 2) {
        // Need at least 2 honest nodes to check agreement
        return violations;
    }

    // Build chain maps: node_id → (height → block_hash)
    auto chain_maps = buildChainMaps(trace, honest_nodes);

    if (chain_maps.empty()) {
        // No chains to compare
        return violations;
    }

    // Get minimum height across all honest nodes
    uint32_t min_height = getMinHeight(chain_maps);

    if (min_height == 0) {
        // All nodes still at genesis
        return violations;
    }

    // Get finalization cutoff (heights beyond this are not yet finalized)
    uint32_t finalization_cutoff = getFinalizationCutoff(chain_maps);

    // Check agreement at each finalized height
    for (uint32_t height = 1; height <= finalization_cutoff; height++) {
        std::vector<NodeID> disagreeing_nodes;

        if (!checkAgreementAtHeight(height, chain_maps, disagreeing_nodes)) {
            // Violation found!
            std::ostringstream desc;
            desc << "Honest nodes disagree at height " << height << ". ";
            desc << "Disagreeing nodes: ";
            for (size_t i = 0; i < disagreeing_nodes.size(); i++) {
                if (i > 0) desc << ", ";
                desc << disagreeing_nodes[i];

                // Show what block they have
                auto it = chain_maps.find(disagreeing_nodes[i]);
                if (it != chain_maps.end()) {
                    auto height_it = it->second.find(height);
                    if (height_it != it->second.end()) {
                        desc << " (block: " << height_it->second << ")";
                    }
                }
            }

            Violation v(getName(), desc.str(), trace.end_time);
            v.involved_nodes = disagreeing_nodes;
            v.details = "Height " + std::to_string(height) + " shows disagreement";
            violations.push_back(v);
        }
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::map<NodeID, std::map<uint32_t, std::string>> DC1Oracle::buildChainMaps(
    const ConsensusTrace& trace,
    const std::vector<NodeID>& honest_nodes
) const {
    std::map<NodeID, std::map<uint32_t, std::string>> chain_maps;

    for (const auto& node_id : honest_nodes) {
        // Get all BLOCK_ACCEPTED events for this node
        auto accepted_events = getBlockAcceptedEvents(trace, node_id);

        // Build height → block_hash map from events
        std::map<uint32_t, std::string> height_map;
        for (const auto& event : accepted_events) {
            if (event.block_height && event.block_hash) {
                // Record block at this height
                height_map[*event.block_height] = *event.block_hash;
            }
        }

        if (!height_map.empty()) {
            chain_maps[node_id] = height_map;
        }
    }

    return chain_maps;
}

uint32_t DC1Oracle::getMinHeight(
    const std::map<NodeID, std::map<uint32_t, std::string>>& chain_maps
) const {
    if (chain_maps.empty()) {
        return 0;
    }

    uint32_t min_height = UINT32_MAX;

    for (const auto& [node_id, height_map] : chain_maps) {
        if (height_map.empty()) {
            min_height = 0;
            break;
        }

        // Find max height for this node
        uint32_t node_max_height = 0;
        for (const auto& [height, block_hash] : height_map) {
            if (height > node_max_height) {
                node_max_height = height;
            }
        }

        if (node_max_height < min_height) {
            min_height = node_max_height;
        }
    }

    return (min_height == UINT32_MAX) ? 0 : min_height;
}

uint32_t DC1Oracle::getFinalizationCutoff(
    const std::map<NodeID, std::map<uint32_t, std::string>>& chain_maps
) const {
    uint32_t min_height = getMinHeight(chain_maps);

    if (min_height < finalization_depth_) {
        return 0;  // Nothing finalized yet
    }

    return min_height - finalization_depth_;
}

bool DC1Oracle::checkAgreementAtHeight(
    uint32_t height,
    const std::map<NodeID, std::map<uint32_t, std::string>>& chain_maps,
    std::vector<NodeID>& disagreeing_nodes
) const {
    disagreeing_nodes.clear();

    // Collect all block hashes at this height
    std::string reference_hash;
    std::vector<NodeID> nodes_at_height;

    for (const auto& [node_id, height_map] : chain_maps) {
        auto it = height_map.find(height);
        if (it != height_map.end()) {
            const std::string& block_hash = it->second;
            nodes_at_height.push_back(node_id);

            if (reference_hash.empty()) {
                // First node - set reference
                reference_hash = block_hash;
            } else if (block_hash != reference_hash) {
                // Disagreement found!
                disagreeing_nodes.push_back(node_id);
            }
        }
    }

    // If we found disagreements, also add the node with the reference hash
    if (!disagreeing_nodes.empty() && !nodes_at_height.empty()) {
        disagreeing_nodes.insert(disagreeing_nodes.begin(), nodes_at_height[0]);
    }

    return disagreeing_nodes.empty();
}

} // namespace test
} // namespace consensus
} // namespace dinero
