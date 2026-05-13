#include "consensus_liveness_oracle_dl1.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<LivenessViolation> DL1Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<LivenessViolation> violations;

    // Check if there was a partition that healed
    auto heal_time = getPartitionHealTime(trace);

    if (!heal_time) {
        // No partition heal event - nothing to check
        return violations;
    }

    // Get honest nodes
    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.size() < 2) {
        // Need at least 2 honest nodes to check convergence
        return violations;
    }

    // Check if nodes converged by the deadline
    uint64_t deadline = *heal_time + convergence_timeout_;

    if (trace.end_time < deadline) {
        // Trace didn't run long enough to verify convergence
        // This is not a violation - we just can't verify yet
        return violations;
    }

    // Check if nodes converged by deadline
    if (!didConvergeByDeadline(trace, deadline)) {
        // Violation: nodes did not converge within timeout
        std::ostringstream desc;
        desc << "Nodes did not converge within " << convergence_timeout_
             << "ms after partition healed at T=" << *heal_time << ". ";
        desc << "Deadline was T=" << deadline << ", trace ended at T=" << trace.end_time << ".";

        // Find which nodes are at different states
        std::vector<std::string> diverging_nodes;
        std::string reference_tip;

        for (const auto& node_id : honest_nodes) {
            auto final_state = getFinalState(trace, node_id);
            if (final_state) {
                if (reference_tip.empty()) {
                    reference_tip = final_state->chain_tip_hash;
                } else if (final_state->chain_tip_hash != reference_tip) {
                    diverging_nodes.push_back(node_id + " (tip: " + final_state->chain_tip_hash + ")");
                }
            }
        }

        LivenessViolation v(getName(), desc.str(), deadline, trace.end_time);
        v.involved_nodes = honest_nodes;
        v.details = "Diverging nodes: " + (diverging_nodes.empty() ? "none" : std::to_string(diverging_nodes.size()));
        violations.push_back(v);
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

bool DL1Oracle::didConvergeByDeadline(
    const ConsensusTrace& trace,
    uint64_t deadline
) const {
    // For Phase 5c simplified implementation:
    // Check if nodes are converged at the end of the trace
    // (which should be >= deadline if we got here)

    return haveNodesConverged(trace);
}

std::optional<uint64_t> DL1Oracle::getConvergenceTime(const ConsensusTrace& trace) const {
    // For Phase 5c, we check convergence at trace end
    // Full implementation would track convergence throughout trace timeline

    if (haveNodesConverged(trace)) {
        return trace.end_time;
    }

    return std::nullopt;
}

} // namespace test
} // namespace consensus
} // namespace dinero
