#include "consensus_partition_tolerance_oracle_dn2.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<PartitionViolation> DN2Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<PartitionViolation> violations;

    // Get partition timeline
    auto partition_start = getPartitionStartTime(trace);
    auto partition_heal = getPartitionHealTime(trace);

    if (!partition_start || !partition_heal) {
        // No partition or no heal - nothing to check
        return violations;
    }

    // Get partition groups
    auto partitions = getPartitionGroups(trace);
    if (partitions.empty()) {
        return violations;
    }

    // Identify majority and minority partitions
    auto majority = getMajorityPartition(trace, partitions);
    auto minorities = getMinorityPartitions(trace, partitions);

    if (!majority || minorities.empty()) {
        // No clear majority/minority split
        return violations;
    }

    // For Phase 5d simplified implementation:
    // Check if all honest nodes converged to the same chain after healing
    // (If minority blocks persisted, nodes wouldn't converge)

    auto honest_nodes = getHonestNodes(trace);
    bool converged = haveNodesConverged(trace, honest_nodes);

    if (!converged) {
        // Violation: nodes didn't converge, suggesting minority blocks survived
        std::ostringstream desc;
        desc << "Nodes did not converge after partition heal at T=" << *partition_heal
             << ". Minority partition blocks may have persisted instead of being orphaned.";

        PartitionViolation v(
            getName(),
            desc.str(),
            *partition_start,
            *partition_heal
        );

        // Include minority partition nodes
        for (const auto& minority : minorities) {
            for (const auto& node_id : minority) {
                v.involved_nodes.push_back(node_id);
            }
        }

        v.details = "Minority blocks survived (nodes diverged)";
        violations.push_back(v);
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
