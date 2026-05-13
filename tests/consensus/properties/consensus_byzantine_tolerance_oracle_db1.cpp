#include "consensus_byzantine_tolerance_oracle_db1.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<ByzantineViolation> DB1Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<ByzantineViolation> violations;

    // Check if Byzantine nodes exist
    auto byzantine_nodes = getByzantineNodes(trace);
    if (byzantine_nodes.empty()) {
        // No Byzantine nodes - property trivially holds
        return violations;
    }

    // Get when first Byzantine node appeared
    auto byzantine_start = getByzantineStartTime(trace);
    if (!byzantine_start) {
        // No Byzantine activity detected - property trivially holds
        return violations;
    }

    // Check if network produced blocks after Byzantine nodes appeared
    bool network_produced_blocks = didNetworkProduceBlocksAfterByzantine(
        trace,
        *byzantine_start
    );

    if (!network_produced_blocks) {
        // Violation: Byzantine nodes present but no blocks produced
        std::ostringstream desc;
        desc << "No blocks produced after Byzantine nodes appeared at T=" << *byzantine_start
             << ". Network stalled in presence of " << byzantine_nodes.size() << " Byzantine node(s).";

        ByzantineViolation v(
            getName(),
            desc.str(),
            *byzantine_start
        );

        // Report which nodes were Byzantine
        for (const auto& node_id : byzantine_nodes) {
            v.involved_nodes.push_back(node_id);
        }

        v.details = "Complete network stall with Byzantine nodes present";
        violations.push_back(v);
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::optional<uint64_t> DB1Oracle::getByzantineStartTime(const ConsensusTrace& trace) const {
    std::optional<uint64_t> earliest_time;

    // Find earliest snapshot where is_byzantine=true
    for (const auto& snapshot : trace.snapshots) {
        if (snapshot.is_byzantine) {
            if (!earliest_time || snapshot.timestamp < *earliest_time) {
                earliest_time = snapshot.timestamp;
            }
        }
    }

    return earliest_time;
}

bool DB1Oracle::didNetworkProduceBlocksAfterByzantine(
    const ConsensusTrace& trace,
    uint64_t byzantine_start_time
) const {
    // Check if ANY BLOCK_ACCEPTED events occurred after Byzantine nodes appeared
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.timestamp >= byzantine_start_time &&
            event.success) {
            return true;  // At least one block produced
        }
    }

    return false;  // No blocks produced
}

} // namespace test
} // namespace consensus
} // namespace dinero
