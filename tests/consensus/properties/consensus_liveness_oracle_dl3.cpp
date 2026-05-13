#include "consensus_liveness_oracle_dl3.h"
#include <sstream>
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

std::vector<LivenessViolation> DL3Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<LivenessViolation> violations;

    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.empty()) {
        return violations;
    }

    // For Phase 5c simplified implementation:
    // Check if chain grew at all during the trace
    // Full implementation would track all stall periods

    // Get initial and final heights
    uint32_t initial_height = 0;
    uint32_t final_height = 0;

    // Find first height
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_height.has_value()) {
            initial_height = *event.block_height;
            break;
        }
    }

    // Find max final height across all honest nodes
    for (const auto& node_id : honest_nodes) {
        auto final_state = getFinalState(trace, node_id);
        if (final_state && final_state->chain_height > final_height) {
            final_height = final_state->chain_height;
        }
    }

    // Check if trace was long enough to expect growth
    uint64_t trace_duration = trace.end_time - trace.start_time;

    if (trace_duration < growth_timeout_) {
        // Trace too short to verify growth
        return violations;
    }

    // If no growth occurred during a long-enough trace, report violation
    if (final_height <= initial_height && trace_duration >= growth_timeout_) {
        std::ostringstream desc;
        desc << "Chain height stalled at " << initial_height
             << " for " << trace_duration << "ms (timeout=" << growth_timeout_ << "ms). "
             << "No blocks produced by any honest node.";

        LivenessViolation v(
            getName(),
            desc.str(),
            trace.start_time + growth_timeout_,
            trace.end_time
        );
        v.involved_nodes = honest_nodes;
        v.details = "Height: " + std::to_string(initial_height) + " (no growth)";
        violations.push_back(v);
    }

    return violations;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

uint32_t DL3Oracle::getMaxHeightAtTime(
    const ConsensusTrace& trace,
    uint64_t timestamp,
    const std::vector<NodeID>& honest_nodes
) const {
    uint32_t max_height = 0;

    // Find max height across all honest nodes at or before timestamp
    for (const auto& event : trace.events) {
        if (event.timestamp > timestamp) {
            break;  // Past target time
        }

        if (event.type == ConsensusEventType::BLOCK_ACCEPTED &&
            event.block_height.has_value()) {
            // Check if this is an honest node
            bool is_honest = std::find(honest_nodes.begin(), honest_nodes.end(), event.node_id)
                             != honest_nodes.end();
            if (is_honest && *event.block_height > max_height) {
                max_height = *event.block_height;
            }
        }
    }

    return max_height;
}

std::vector<std::pair<uint64_t, uint64_t>> DL3Oracle::findStallPeriods(
    const ConsensusTrace& trace,
    const std::vector<NodeID>& honest_nodes
) const {
    std::vector<std::pair<uint64_t, uint64_t>> stall_periods;

    // For Phase 5c: Simplified implementation
    // Full implementation would track all stall periods throughout trace
    // For now, just check overall growth (implemented in observeTrace)

    return stall_periods;
}

} // namespace test
} // namespace consensus
} // namespace dinero
