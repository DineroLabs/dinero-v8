#include "consensus_types.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

// ============================================================================
// NetworkTopology Implementation
// ============================================================================

NetworkTopology NetworkTopology::fullMesh(const std::vector<NodeID>& nodes) {
    NetworkTopology topology;
    topology.type = TopologyType::FULL_MESH;
    topology.nodes = nodes;

    // Connect every node to every other node
    for (const auto& node : nodes) {
        std::vector<NodeID> peers;
        for (const auto& peer : nodes) {
            if (peer != node) {
                peers.push_back(peer);
            }
        }
        topology.connections[node] = peers;
    }

    return topology;
}

NetworkTopology NetworkTopology::star(const std::vector<NodeID>& nodes) {
    NetworkTopology topology;
    topology.type = TopologyType::STAR;
    topology.nodes = nodes;

    if (nodes.empty()) {
        return topology;
    }

    const NodeID& hub = nodes[0];

    // Hub connects to all spokes
    std::vector<NodeID> hub_peers;
    for (size_t i = 1; i < nodes.size(); i++) {
        hub_peers.push_back(nodes[i]);
    }
    topology.connections[hub] = hub_peers;

    // Each spoke connects only to hub
    for (size_t i = 1; i < nodes.size(); i++) {
        topology.connections[nodes[i]] = {hub};
    }

    return topology;
}

NetworkTopology NetworkTopology::chain(const std::vector<NodeID>& nodes) {
    NetworkTopology topology;
    topology.type = TopologyType::CHAIN;
    topology.nodes = nodes;

    if (nodes.empty()) {
        return topology;
    }

    // Linear chain: n0→n1→n2→...→nN
    for (size_t i = 0; i < nodes.size(); i++) {
        std::vector<NodeID> peers;

        // Connect to previous node (if exists)
        if (i > 0) {
            peers.push_back(nodes[i - 1]);
        }

        // Connect to next node (if exists)
        if (i + 1 < nodes.size()) {
            peers.push_back(nodes[i + 1]);
        }

        topology.connections[nodes[i]] = peers;
    }

    return topology;
}

} // namespace test
} // namespace consensus
} // namespace dinero
