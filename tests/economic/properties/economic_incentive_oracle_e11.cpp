#include "economic_incentive_oracle_e11.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicIncentiveViolation> E11Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicIncentiveViolation> violations;

    // Get all TX_SELECTED_FOR_BLOCK and TX_EXCLUDED_FROM_BLOCK events
    auto selected_events = getEventsByType(trace, EconomicEventType::TX_SELECTED_FOR_BLOCK);
    auto excluded_events = getEventsByType(trace, EconomicEventType::TX_EXCLUDED_FROM_BLOCK);

    // For each excluded transaction, check if it has higher fee rate than any included tx
    for (const auto& excluded : excluded_events) {
        if (!excluded.fee_rate || !excluded.tx_id) {
            continue;
        }

        double excluded_fee_rate = *excluded.fee_rate;

        // Find included txs with lower fee rates
        for (const auto& selected : selected_events) {
            if (!selected.fee_rate || !selected.tx_id) {
                continue;
            }

            double selected_fee_rate = *selected.fee_rate;

            // Check if excluded tx has higher fee rate than selected tx
            // This violates mining incentive compatibility (miner leaving money on table)
            if (excluded_fee_rate > selected_fee_rate) {
                EconomicIncentiveViolation violation(
                    getName(),
                    "Miner excluded higher-fee transaction while including lower-fee transaction",
                    excluded.timestamp
                );
                violation.tx_id = excluded.tx_id;
                violation.involved_nodes.push_back(excluded.node_id);
                violation.details = "Excluded tx " + *excluded.tx_id +
                                   " (fee rate: " + std::to_string(excluded_fee_rate) + " sat/byte)" +
                                   " has higher fee than included tx " + *selected.tx_id +
                                   " (fee rate: " + std::to_string(selected_fee_rate) + " sat/byte)." +
                                   " Miner is not maximizing fee revenue.";
                violations.push_back(violation);

                // Only report one violation per excluded tx
                break;
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
