#include "economic_incentive_oracle_e14.h"
#include <set>

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicIncentiveViolation> E14Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicIncentiveViolation> violations;

    // Get all evicted transactions with their fee rates
    auto evicted_events = getEventsByType(trace, EconomicEventType::TX_EVICTED_MEMPOOL);

    // Get all accepted transactions (these entered mempool)
    auto accepted_txs = getAcceptedTxs(trace);

    // Get all confirmed transactions (these left mempool successfully)
    std::set<std::string> confirmed_txs_set;
    for (const auto& tx : getConfirmedTxs(trace)) {
        confirmed_txs_set.insert(tx);
    }

    // Get all evicted transactions
    std::set<std::string> evicted_txs_set;
    for (const auto& event : evicted_events) {
        if (event.tx_id) {
            evicted_txs_set.insert(*event.tx_id);
        }
    }

    // For each evicted transaction, check if any remaining tx has lower fee rate
    for (const auto& evicted_event : evicted_events) {
        if (!evicted_event.tx_id || !evicted_event.fee_rate) {
            continue;
        }

        double evicted_fee_rate = *evicted_event.fee_rate;

        // Check all accepted transactions
        for (const auto& tx_id : accepted_txs) {
            // Skip if this tx was evicted or confirmed (not remaining)
            if (evicted_txs_set.count(tx_id) > 0 || confirmed_txs_set.count(tx_id) > 0) {
                continue;
            }

            // Get fee rate for this remaining tx
            auto remaining_fee_rate = getTxFeeRate(trace, tx_id);
            if (!remaining_fee_rate) {
                continue;
            }

            // Check if evicted tx has higher fee rate than remaining tx
            // This violates spam prevention (high-fee tx evicted, low-fee tx remains)
            if (evicted_fee_rate > *remaining_fee_rate) {
                EconomicIncentiveViolation violation(
                    getName(),
                    "High-fee transaction evicted while low-fee transaction remains in mempool",
                    evicted_event.timestamp
                );
                violation.tx_id = evicted_event.tx_id;
                violation.involved_nodes.push_back(evicted_event.node_id);
                violation.details = "Evicted tx " + *evicted_event.tx_id +
                                   " (fee rate: " + std::to_string(evicted_fee_rate) + " sat/byte)" +
                                   " has higher fee than remaining tx " + tx_id +
                                   " (fee rate: " + std::to_string(*remaining_fee_rate) + " sat/byte)." +
                                   " Mempool eviction policy allows spam to crowd out legitimate txs.";
                violations.push_back(violation);

                // Only report one violation per evicted tx
                break;
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
