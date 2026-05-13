#include "economic_liveness_oracle_e10.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicLivenessViolation> E10Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicLivenessViolation> violations;

    // Get all confirmed transactions
    auto confirmed_events = getEventsByType(trace, EconomicEventType::TX_INCLUDED_IN_BLOCK);

    // Get all reorged out transactions
    auto reorg_events = getEventsByType(trace, EconomicEventType::TX_REORGED_OUT);

    // Check if any confirmed tx later reorged out
    for (const auto& confirmed : confirmed_events) {
        if (!confirmed.tx_id) {
            continue;
        }

        for (const auto& reorg : reorg_events) {
            if (!reorg.tx_id) {
                continue;
            }

            // Check if same transaction
            if (*confirmed.tx_id == *reorg.tx_id) {
                // Check if reorg happened after confirmation
                if (reorg.timestamp >= confirmed.timestamp) {
                    EconomicLivenessViolation violation(
                        getName(),
                        "Confirmed transaction later reorged out",
                        reorg.timestamp
                    );
                    violation.tx_id = confirmed.tx_id;
                    violation.involved_nodes.push_back(confirmed.node_id);
                    violation.details = "Transaction confirmed at " +
                                       std::to_string(confirmed.timestamp) +
                                       " but reorged out at " +
                                       std::to_string(reorg.timestamp);

                    if (confirmed.block_height && reorg.block_height) {
                        violation.details += " (confirmed at height " +
                                           std::to_string(*confirmed.block_height) +
                                           ", reorged at height " +
                                           std::to_string(*reorg.block_height) + ")";
                    }

                    violations.push_back(violation);
                }
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
