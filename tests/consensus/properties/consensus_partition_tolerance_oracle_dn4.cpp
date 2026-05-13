#include "consensus_partition_tolerance_oracle_dn4.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<PartitionViolation> DN4Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<PartitionViolation> violations;

    // Get partition timeline
    auto partition_start = getPartitionStartTime(trace);
    auto partition_heal = getPartitionHealTime(trace);

    if (!partition_start || !partition_heal) {
        // No partition or no heal - nothing to check
        return violations;
    }

    // For Phase 5d simplified implementation:
    // Check if all honest nodes converged to the same final state
    // (Asynchronous healing should still result in deterministic convergence)

    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.empty()) {
        return violations;
    }

    // Check convergence
    bool converged = haveNodesConverged(trace, honest_nodes);

    if (!converged) {
        // Violation: nodes didn't converge, suggesting non-deterministic outcome
        std::ostringstream desc;
        desc << "Nodes did not converge after partition healed at T=" << *partition_heal
             << ". Different nodes reached different final states, "
             << "suggesting healing order affected the outcome (non-deterministic).";

        PartitionViolation v(
            getName(),
            desc.str(),
            *partition_start,
            *partition_heal
        );
        v.involved_nodes = honest_nodes;
        v.details = "Non-deterministic healing outcome";
        violations.push_back(v);
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
