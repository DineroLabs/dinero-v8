#include "consensus_trace.h"
#include <algorithm>
#include <functional>
#include <set>

namespace dinero {
namespace consensus {
namespace test {

// Simple hash combiner (FNV-1a inspired)
static uint64_t hashCombine(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

static uint64_t hashString(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

uint64_t ConsensusTrace::computeHash() const {
    uint64_t hash = rng_seed;

    // Hash scenario name
    hash = hashCombine(hash, hashString(scenario_name));

    // Hash topology
    hash = hashCombine(hash, static_cast<uint64_t>(topology.type));
    for (const auto& node : topology.nodes) {
        hash = hashCombine(hash, hashString(node));
    }

    // Hash all actions
    for (const auto& action : actions) {
        hash = hashCombine(hash, static_cast<uint64_t>(action.type));
        hash = hashCombine(hash, action.timestamp);
        hash = hashCombine(hash, action.sequence_number);

        if (action.node_id) {
            hash = hashCombine(hash, hashString(*action.node_id));
        }
        if (action.latency_ms) {
            hash = hashCombine(hash, *action.latency_ms);
        }
        if (action.block_hash) {
            hash = hashCombine(hash, hashString(*action.block_hash));
        }
    }

    // Hash all events
    for (const auto& event : events) {
        hash = hashCombine(hash, static_cast<uint64_t>(event.type));
        hash = hashCombine(hash, event.timestamp);
        hash = hashCombine(hash, event.sequence_number);
        hash = hashCombine(hash, hashString(event.node_id));
        hash = hashCombine(hash, event.success ? 1 : 0);

        if (event.block_hash) {
            hash = hashCombine(hash, hashString(*event.block_hash));
        }
        if (event.block_height) {
            hash = hashCombine(hash, *event.block_height);
        }
        if (event.chainwork) {
            hash = hashCombine(hash, *event.chainwork);
        }
    }

    // Hash all state snapshots
    for (const auto& snapshot : snapshots) {
        hash = hashCombine(hash, hashString(snapshot.node_id));
        hash = hashCombine(hash, snapshot.timestamp);
        hash = hashCombine(hash, hashString(snapshot.chain_tip_hash));
        hash = hashCombine(hash, snapshot.chain_height);
        hash = hashCombine(hash, snapshot.chainwork);
        hash = hashCombine(hash, static_cast<uint64_t>(snapshot.mining_phase));
        hash = hashCombine(hash, snapshot.is_byzantine ? 1 : 0);
    }

    return hash;
}

std::vector<ConsensusEvent> ConsensusTrace::getEventsForNode(const NodeID& node_id) const {
    std::vector<ConsensusEvent> node_events;
    for (const auto& event : events) {
        if (event.node_id == node_id) {
            node_events.push_back(event);
        }
    }
    return node_events;
}

std::vector<ConsensusState> ConsensusTrace::getSnapshotsForNode(const NodeID& node_id) const {
    std::vector<ConsensusState> node_snapshots;
    for (const auto& snapshot : snapshots) {
        if (snapshot.node_id == node_id) {
            node_snapshots.push_back(snapshot);
        }
    }
    return node_snapshots;
}

std::optional<ConsensusState> ConsensusTrace::getStateAt(const NodeID& node_id, uint64_t timestamp) const {
    std::optional<ConsensusState> result;

    // Find the latest snapshot before or at the given timestamp
    for (const auto& snapshot : snapshots) {
        if (snapshot.node_id == node_id && snapshot.timestamp <= timestamp) {
            if (!result || snapshot.timestamp > result->timestamp) {
                result = snapshot;
            }
        }
    }

    return result;
}

std::vector<NodeID> ConsensusTrace::getAllNodes() const {
    std::set<NodeID> unique_nodes;

    // Collect from topology
    for (const auto& node : topology.nodes) {
        unique_nodes.insert(node);
    }

    // Collect from events
    for (const auto& event : events) {
        unique_nodes.insert(event.node_id);
    }

    // Collect from snapshots
    for (const auto& snapshot : snapshots) {
        unique_nodes.insert(snapshot.node_id);
    }

    return std::vector<NodeID>(unique_nodes.begin(), unique_nodes.end());
}

} // namespace test
} // namespace consensus
} // namespace dinero
