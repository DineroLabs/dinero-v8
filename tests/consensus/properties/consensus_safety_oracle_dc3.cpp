#include "consensus_safety_oracle_dc3.h"
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace dinero {
namespace consensus {
namespace test {

std::vector<Violation> DC3Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<Violation> violations;

    const auto honest_nodes = getHonestNodes(trace);
    const std::set<NodeID> honest(honest_nodes.begin(), honest_nodes.end());
    std::set<NodeID> observed_nodes;
    // A finalized spend of the same outpoint by different transactions cannot
    // survive in two honest nodes' selected histories (or twice in one).
    std::map<std::string, std::pair<std::string, NodeID>> spent_by;
    std::size_t observed_spends = 0;

    for (const auto& block : trace.finalized_blocks) {
        if (!honest.count(block.node_id)) continue;
        observed_nodes.insert(block.node_id);
        for (const auto& spend : block.spends) {
            if (spend.tx_id.empty() || spend.outpoint.empty()) {
                Violation v(getName(), "Finalized spend lacks transaction or outpoint identity");
                v.involved_nodes.push_back(block.node_id);
                violations.push_back(std::move(v));
                continue;
            }
            ++observed_spends;
            const auto [it, inserted] = spent_by.emplace(
                spend.outpoint, std::make_pair(spend.tx_id, block.node_id));
            if (!inserted && it->second.first != spend.tx_id) {
                Violation v(getName(), "Conflicting finalized spends of outpoint " + spend.outpoint);
                v.involved_nodes = {it->second.second, block.node_id};
                v.details = it->second.first + " conflicts with " + spend.tx_id;
                violations.push_back(std::move(v));
            }
        }
    }

    // The Phase-5b simulator records mempool acceptance and opaque block
    // hashes, not selected transaction inputs. Empty input evidence is unknown,
    // never a DC3 pass.
    if (honest.empty() || observed_nodes.size() != honest.size() || observed_spends == 0) {
        Violation v(getName(), "Insufficient finalized spend evidence for honest nodes");
        v.involved_nodes = honest_nodes;
        violations.push_back(std::move(v));
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
