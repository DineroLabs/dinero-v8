#include "economic_incentive_oracle_e12.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicIncentiveViolation> E12Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicIncentiveViolation> violations;

    // Get all confirmed transactions with their fee rates and confirmation times
    auto confirmed_events = getEventsByType(trace, EconomicEventType::TX_INCLUDED_IN_BLOCK);

    // For each pair of confirmed transactions, check if lower-fee tx confirmed before higher-fee tx
    for (size_t i = 0; i < confirmed_events.size(); ++i) {
        const auto& tx1 = confirmed_events[i];
        if (!tx1.tx_id || !tx1.fee_rate) {
            continue;
        }

        for (size_t j = i + 1; j < confirmed_events.size(); ++j) {
            const auto& tx2 = confirmed_events[j];
            if (!tx2.tx_id || !tx2.fee_rate) {
                continue;
            }

            // Check if lower-fee tx confirmed before higher-fee tx
            // tx1 confirmed at tx1.timestamp
            // tx2 confirmed at tx2.timestamp
            // If tx1.fee_rate < tx2.fee_rate AND tx1.timestamp < tx2.timestamp
            // This violates fee market efficiency (lower fee confirmed first)
            if (*tx1.fee_rate < *tx2.fee_rate && tx1.timestamp < tx2.timestamp) {
                EconomicIncentiveViolation violation(
                    getName(),
                    "Low-fee transaction confirmed before high-fee transaction",
                    tx2.timestamp
                );
                violation.tx_id = tx2.tx_id;
                violation.involved_nodes.push_back(tx1.node_id);
                violation.details = "Lower-fee tx " + *tx1.tx_id +
                                   " (fee rate: " + std::to_string(*tx1.fee_rate) + " sat/byte)" +
                                   " confirmed at timestamp " + std::to_string(tx1.timestamp) +
                                   " before higher-fee tx " + *tx2.tx_id +
                                   " (fee rate: " + std::to_string(*tx2.fee_rate) + " sat/byte)" +
                                   " confirmed at timestamp " + std::to_string(tx2.timestamp) + "." +
                                   " Fee market is not efficient.";
                violations.push_back(violation);
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
