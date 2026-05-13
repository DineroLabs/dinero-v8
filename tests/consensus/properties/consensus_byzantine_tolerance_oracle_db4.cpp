#include "consensus_byzantine_tolerance_oracle_db4.h"
#include <sstream>

namespace dinero {
namespace consensus {
namespace test {

std::vector<ByzantineViolation> DB4Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<ByzantineViolation> violations;

    // Check if block withholding occurred
    auto withholding_start = getBlockWithholdingStartTime(trace);
    if (!withholding_start) {
        // No block withholding detected - property trivially holds
        return violations;
    }

    // Count withheld blocks
    size_t withheld_count = countWithheldBlocks(trace);

    // Check if network produced blocks after withholding began
    bool network_produced_blocks = didNetworkProduceBlocksAfterWithholding(
        trace,
        *withholding_start
    );

    if (!network_produced_blocks) {
        // Violation: Block withholding caused network stall
        std::ostringstream desc;
        desc << "No blocks produced after block withholding began at T=" << *withholding_start
             << ". " << withheld_count << " block(s) withheld. Network stalled.";

        ByzantineViolation v(
            getName(),
            desc.str(),
            *withholding_start
        );

        // Report Byzantine nodes that withheld blocks
        auto byzantine_nodes = getByzantineNodes(trace);
        for (const auto& node_id : byzantine_nodes) {
            v.involved_nodes.push_back(node_id);
        }

        std::ostringstream details;
        details << "Block withholding attack succeeded: " << withheld_count
                << " blocks withheld, network failed to progress";
        v.details = details.str();

        violations.push_back(v);
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

std::optional<uint64_t> DB4Oracle::getBlockWithholdingStartTime(
    const ConsensusTrace& trace
) const {
    std::optional<uint64_t> earliest_time;

    // Look for WITHHOLD_BLOCK actions
    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::WITHHOLD_BLOCK) {
            if (!earliest_time || action.timestamp < *earliest_time) {
                earliest_time = action.timestamp;
            }
        }
    }

    return earliest_time;
}

bool DB4Oracle::didNetworkProduceBlocksAfterWithholding(
    const ConsensusTrace& trace,
    uint64_t withholding_start_time
) const {
    // Check if ANY BLOCK_ACCEPTED events occurred after withholding started
    // Only count blocks from honest nodes (not Byzantine)
    auto honest_nodes = getHonestNodes(trace);

    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.timestamp >= withholding_start_time &&
            event.success) {

            // Check if this block was produced by an honest node
            // (We want to verify honest nodes continued making progress)
            bool is_honest_block = false;
            for (const auto& honest_node : honest_nodes) {
                if (event.node_id == honest_node) {
                    is_honest_block = true;
                    break;
                }
            }

            if (is_honest_block) {
                return true;  // Honest node produced a block
            }
        }
    }

    return false;  // No blocks produced by honest nodes
}

size_t DB4Oracle::countWithheldBlocks(const ConsensusTrace& trace) const {
    size_t count = 0;

    for (const auto& action : trace.actions) {
        if (action.type == ConsensusActionType::WITHHOLD_BLOCK) {
            count++;
        }
    }

    return count;
}

} // namespace test
} // namespace consensus
} // namespace dinero
