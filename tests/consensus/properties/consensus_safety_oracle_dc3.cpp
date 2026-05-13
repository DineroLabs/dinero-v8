#include "consensus_safety_oracle_dc3.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

std::vector<Violation> DC3Oracle::observeTrace(const ConsensusTrace& trace) {
    std::vector<Violation> violations;

    // Get honest nodes
    auto honest_nodes = getHonestNodes(trace);

    if (honest_nodes.size() < 2) {
        return violations;
    }

    // Simplified check for Phase 5b:
    // Look for TX_ACCEPTED events with duplicate tx_ids across different nodes
    // This would indicate a potential double-spend if they conflict

    std::map<std::string, std::vector<NodeID>> tx_acceptances;

    for (const auto& event : trace.events) {
        if (event.type == ConsensusEventType::TX_ACCEPTED && event.tx_id) {
            bool is_honest = std::find(honest_nodes.begin(), honest_nodes.end(), event.node_id) != honest_nodes.end();

            if (is_honest) {
                tx_acceptances[*event.tx_id].push_back(event.node_id);
            }
        }
    }

    // For Phase 5b, we just ensure no obvious integrity violations
    // Full UTXO conflict detection will be added in Phase 5c+

    return violations;
}

} // namespace test
} // namespace consensus
} // namespace dinero
