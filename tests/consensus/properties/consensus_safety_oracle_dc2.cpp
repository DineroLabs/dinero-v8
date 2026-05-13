#include "consensus_safety_oracle_dc2.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

std::vector<Violation> DC2Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<Violation> violations;

    // Get honest nodes
    auto honest_nodes = getHonestNodes(trace);

    // Check for BLOCK_REJECTED events on honest nodes
    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::BLOCK_REJECTED) {
            // Check if this is an honest node
            bool is_honest = std::find(honest_nodes.begin(), honest_nodes.end(), event.node_id) != honest_nodes.end();

            if (is_honest) {
                // Honest node rejected a block - this might indicate invalid block was proposed
                // For Phase 5b, we consider this acceptable (e.g., stale blocks, orphans)
                // Full validation in Phase 5c+ will check transaction validity
            }
        }

        // Check for BLOCK_ACCEPTED with success=false (shouldn't happen)
        if (event.type == ConsensusEventType::BLOCK_ACCEPTED && !event.success) {
            bool is_honest = std::find(honest_nodes.begin(), honest_nodes.end(), event.node_id) != honest_nodes.end();

            if (is_honest) {
                Violation v(getName(), "Honest node accepted block with success=false", event.timestamp);
                v.involved_nodes.push_back(event.node_id);
                v.details = "Node " + event.node_id + " accepted invalid block";
                violations.push_back(v);
            }
        }
    }

    // Note: Full validation (transactions, subsidy, PoW) will be added in Phase 5c+
    // For Phase 5b foundation, we just check for obvious invalidity indicators

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
