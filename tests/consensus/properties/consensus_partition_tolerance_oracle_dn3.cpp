#include "consensus_partition_tolerance_oracle_dn3.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<PartitionViolation> DN3Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<PartitionViolation> violations;

    // Get partition timeline
    auto partition_start = getPartitionStartTime(trace);
    auto partition_heal = getPartitionHealTime(trace);

    if (!partition_start || !partition_heal) {
        // No partition or no heal - nothing to check
        return violations;
    }

    // For Phase 5d simplified implementation:
    // Check if all honest nodes reached the same maximum height after healing
    // (If blocks were lost, heights would diverge or stall)

    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.empty()) {
        return violations;
    }

    // Get final heights for all honest nodes
    uint32_t max_height = 0;
    uint32_t min_height = UINT32_MAX;

    for (const auto& node_id : honest_nodes) {
        auto final_state = getFinalState(trace, node_id);
        if (final_state) {
            max_height = std::max(max_height, final_state->chain_height);
            min_height = std::min(min_height, final_state->chain_height);
        }
    }

    // Check if all nodes converged to same height
    // (Clean healing should result in all nodes at same height)
    if (max_height != min_height) {
        std::ostringstream desc;
        desc << "After partition healed at T=" << *partition_heal
             << ", nodes did not reach the same height. "
             << "Max height: " << max_height << ", Min height: " << min_height << ". "
             << "Suggests blocks were lost or not properly propagated during healing.";

        PartitionViolation v(
            getName(),
            desc.str(),
            *partition_start,
            *partition_heal
        );
        v.involved_nodes = honest_nodes;
        v.details = "Height divergence after healing";
        violations.push_back(v);
    }

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
