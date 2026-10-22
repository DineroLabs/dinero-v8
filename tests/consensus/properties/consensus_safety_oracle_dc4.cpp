#include "consensus_safety_oracle_dc4.h"
#include <map>
#include <set>
#include <utility>

namespace dinero {
namespace consensus {
namespace test {

std::vector<Violation> DC4Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<Violation> violations;

    const auto honest_nodes = getHonestNodes(trace);
    const std::set<NodeID> honest(honest_nodes.begin(), honest_nodes.end());
    std::map<NodeID, std::map<uint32_t, const FinalizedBlockObservation*>> histories;
    std::map<std::pair<uint32_t, std::string>, std::pair<std::string, NodeID>> parents;

    for (const auto& block : trace.finalized_blocks) {
        if (!honest.count(block.node_id)) continue;
        if (block.block_hash.empty() || block.parent_hash.empty()) {
            Violation v(getName(), "Finalized block lacks hash or parent identity");
            v.involved_nodes.push_back(block.node_id);
            violations.push_back(std::move(v));
            continue;
        }
        auto& history = histories[block.node_id];
        const auto [at_height, inserted] = history.emplace(block.height, &block);
        if (!inserted && at_height->second->block_hash != block.block_hash) {
            Violation v(getName(), "Honest node has conflicting finalized blocks at one height");
            v.involved_nodes.push_back(block.node_id);
            violations.push_back(std::move(v));
        }
        const auto [prior, first] = parents.emplace(
            std::make_pair(block.height, block.block_hash),
            std::make_pair(block.parent_hash, block.node_id));
        if (!first && prior->second.first != block.parent_hash) {
            Violation v(getName(), "Same finalized block has different parents across honest nodes");
            v.involved_nodes = {prior->second.second, block.node_id};
            v.details = block.block_hash;
            violations.push_back(std::move(v));
        }
    }

    std::set<NodeID> linked_nodes;
    for (const auto& [node, history] : histories) {
        for (const auto& [height, block] : history) {
            if (height == 0) continue;
            const auto parent = history.find(height - 1);
            if (parent == history.end()) continue;
            linked_nodes.insert(node);
            if (block->parent_hash != parent->second->block_hash) {
                Violation v(getName(), "Finalized block does not extend the preceding selected block");
                v.involved_nodes.push_back(node);
                v.details = block->block_hash;
                violations.push_back(std::move(v));
            }
        }
    }
    if (honest.empty() || histories.size() != honest.size() || linked_nodes.size() != honest.size()) {
        Violation v(getName(), "Insufficient finalized parent-link evidence for honest nodes");
        v.involved_nodes = honest_nodes;
        violations.push_back(std::move(v));
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
