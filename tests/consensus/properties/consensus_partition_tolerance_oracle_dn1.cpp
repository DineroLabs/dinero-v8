#include "consensus_partition_tolerance_oracle_dn1.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<PartitionViolation> DN1Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<PartitionViolation> violations;

    // Get partition timeline
    auto partition_start = getPartitionStartTime(trace);
    auto partition_heal = getPartitionHealTime(trace);

    if (!partition_start) {
        // No partition - nothing to check
        return violations;
    }

    // Determine end time for liveness check
    uint64_t check_end_time = partition_heal.value_or(trace.end_time);

    // Check if ANY blocks were produced network-wide during partition
    bool network_produced_blocks = didNetworkProduceBlocks(
        trace,
        *partition_start,
        check_end_time
    );

    if (!network_produced_blocks) {
        // Violation: complete network stall during partition
        std::ostringstream desc;
        desc << "No blocks produced network-wide during partition from T=" << *partition_start
             << " to T=" << check_end_time << ". "
             << "Complete network liveness failure.";

        PartitionViolation v(
            getName(),
            desc.str(),
            *partition_start,
            partition_heal.value_or(0)
        );
        v.involved_nodes = getHonestNodes(trace);
        v.details = "Total network stall";
        violations.push_back(v);
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

bool DN1Oracle::didNetworkProduceBlocks(
    const ConsensusTrace& trace,
    uint64_t start_time,
    uint64_t end_time
) const {
    // Check if ANY BLOCK_ACCEPTED events occurred during partition period
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.timestamp >= start_time &&
            event.timestamp <= end_time &&
            event.success) {
            return true;  // At least one block produced
        }
    }

    return false;  // No blocks produced
}

} // namespace test
} // namespace consensus
} // namespace dinero
