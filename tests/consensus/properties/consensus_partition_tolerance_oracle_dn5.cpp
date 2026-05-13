#include "consensus_partition_tolerance_oracle_dn5.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<PartitionViolation> DN5Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<PartitionViolation> violations;

    // Count partition cycles
    size_t num_cycles = countPartitionCycles(trace);

    if (num_cycles == 0) {
        // No partitions - nothing to check
        return violations;
    }

    // For Phase 5d simplified implementation:
    // Just check if all nodes converged at the end
    // (Regardless of how many partition cycles occurred)

    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.empty()) {
        return violations;
    }

    // Check final convergence
    bool converged = haveNodesConverged(trace, honest_nodes);

    if (!converged && num_cycles > 1) {
        // Violation: nodes didn't converge after multiple partitions
        std::ostringstream desc;
        desc << "After " << num_cycles << " partition/heal cycle(s), "
             << "nodes did not converge to a consistent state. "
             << "Cascading partitions left network in diverged state.";

        PartitionViolation v(
            getName(),
            desc.str(),
            0,  // Multiple partitions - no single partition time
            trace.end_time
        );
        v.involved_nodes = honest_nodes;
        v.details = "Failed to converge after " + std::to_string(num_cycles) + " cycles";
        violations.push_back(v);
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

size_t DN5Oracle::countPartitionCycles(const ConsensusTrace& trace) const {
    size_t partition_count = 0;
    size_t heal_count = 0;

    // Count PARTITION_ACTIVATED events
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::PARTITION_ACTIVATED) {
            partition_count++;
        } else if (event.type == ConsensusEventType::PARTITION_HEALED) {
            heal_count++;
        }
    }

    // Also check actions
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::PARTITION_NETWORK) {
            partition_count++;
        } else if (action.type == ConsensusActionType::HEAL_PARTITION) {
            heal_count++;
        }
    }

    // A cycle = partition + heal
    // Return min of partition_count and heal_count (complete cycles)
    return std::min(partition_count, heal_count);
}

} // namespace test
} // namespace consensus
} // namespace dinero
