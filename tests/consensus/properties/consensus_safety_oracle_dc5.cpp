#include "consensus_safety_oracle_dc5.h"

namespace dinero {
namespace consensus {
namespace test {

std::vector<Violation> DC5Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<Violation> violations;

    // Get honest nodes
    auto honest_nodes = getHonestNodes(trace);

    for (const auto& node_id : honest_nodes) {
        // Get CHAIN_TIP_CHANGED events for this node (indicating reorgs)
        auto tip_changed_events = getChainTipChangedEvents(trace, node_id);

        if (tip_changed_events.empty()) {
            continue;  // No reorgs on this node
        }

        // Check if any reorg was deeper than finality_depth
        if (detectDeepReorg(tip_changed_events, finality_depth_)) {
            Violation v(getName(), "Deep reorg detected beyond finality threshold", trace.end_time);
            v.involved_nodes.push_back(node_id);
            v.details = "Node " + node_id + " experienced reorg deeper than " +
                       std::to_string(finality_depth_) + " blocks";
            violations.push_back(v);
        }
    }

    return violations;
}

bool DC5Oracle::detectDeepReorg(
    const std::vector<ConsensusEvent>& tip_changed_events,
    uint32_t finality_depth
) const {
    // For Phase 5b, we use a simplified detection:
    // If there are multiple CHAIN_TIP_CHANGED events with large height differences,
    // it might indicate a deep reorg

    if (tip_changed_events.size() < 2) {
        return false;  // Need at least 2 tip changes to detect reorg
    }

    // Track maximum height drop
    uint32_t max_height_drop = 0;
    uint32_t prev_height = 0;

    for (const auto& event : tip_changed_events) {
        if (event.block_height) {
            uint32_t current_height = *event.block_height;

            if (prev_height > 0 && current_height < prev_height) {
                // Height decreased - this is a reorg
                uint32_t drop = prev_height - current_height;
                if (drop > max_height_drop) {
                    max_height_drop = drop;
                }
            }

            prev_height = current_height;
        }
    }

    // If we saw a drop deeper than finality_depth, that's a violation
    return max_height_drop > finality_depth;
}

} // namespace test
} // namespace consensus
} // namespace dinero
