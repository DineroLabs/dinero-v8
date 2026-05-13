#include "consensus_byzantine_tolerance_oracle_db2.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<ByzantineViolation> DB2Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<ByzantineViolation> violations;

    // Check if Byzantine nodes exist
    auto byzantine_nodes = getByzantineNodes(trace);
    if (byzantine_nodes.empty()) {
        // No Byzantine nodes - property trivially holds
        return violations;
    }

    // Check if honest nodes exist
    auto honest_nodes = getHonestNodes(trace);
    if (honest_nodes.size() < 2) {
        // Need at least 2 honest nodes to check convergence
        return violations;
    }

    // Check if all honest nodes converged to same chain tip
    bool converged = haveHonestNodesConverged(trace);

    if (!converged) {
        // Violation: Honest nodes diverged in presence of Byzantine nodes
        std::ostringstream desc;
        desc << "Honest nodes failed to converge with " << byzantine_nodes.size()
             << " Byzantine node(s) present. Eclipse attack succeeded.";

        ByzantineViolation v(
            getName(),
            desc.str(),
            trace.end_time
        );

        // Report which nodes were involved
        for (const auto& node_id : honest_nodes) {
            auto final_state = getFinalState(trace, node_id);
            if (final_state) {
                v.involved_nodes.push_back(node_id);
            }
        }

        // Build detailed report of divergence
        std::ostringstream details;
        details << "Honest nodes diverged: ";
        for (const auto& node_id : honest_nodes) {
            auto final_state = getFinalState(trace, node_id);
            if (final_state) {
                details << node_id << " (height=" << final_state->chain_height
                       << ", tip=" << final_state->chain_tip_hash.substr(0, 8) << "...) ";
            }
        }

        v.details = details.str();
        violations.push_back(v);
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
