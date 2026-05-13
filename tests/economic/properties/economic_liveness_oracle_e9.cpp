#include "economic_liveness_oracle_e9.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicLivenessViolation> E9Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicLivenessViolation> violations;

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
            if (excluded_fee_rate > selected_fee_rate) {
                EconomicLivenessViolation violation(
                    getName(),
                    "Higher-fee transaction excluded while lower-fee transaction included",
                    excluded.timestamp
                );
                violation.tx_id = excluded.tx_id;
                violation.involved_nodes.push_back(excluded.node_id);
                violation.details = "Excluded tx " + *excluded.tx_id +
                                   " (fee rate: " + std::to_string(excluded_fee_rate) + " sat/byte)" +
                                   " has higher fee than included tx " + *selected.tx_id +
                                   " (fee rate: " + std::to_string(selected_fee_rate) + " sat/byte)";
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
