#include "economic_incentive_oracle_e13.h"
#include <map>
#include <vector>

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicIncentiveViolation> E13Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicIncentiveViolation> violations;

    // Build map: tx_id -> (accepted_time, fee_rate, selected_order)
    std::map<std::string, uint64_t> accepted_times;
    std::map<std::string, double> fee_rates;
    std::map<std::string, size_t> selected_order;

    // Get accepted times and fee rates
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_ACCEPTED_TO_MEMPOOL &&
            event.success && event.tx_id && event.fee_rate) {
            accepted_times[*event.tx_id] = event.timestamp;
            fee_rates[*event.tx_id] = *event.fee_rate;
        }
    }

    // Get selection order
    size_t order = 0;
    for (const auto& event : trace.events) {
        if (event.type == EconomicEventType::TX_SELECTED_FOR_BLOCK && event.tx_id) {
            selected_order[*event.tx_id] = order++;
        }
    }

    // Check for reordering of equal-fee transactions
    for (const auto& [tx1_id, tx1_order] : selected_order) {
        // Skip if we don't have accepted time or fee rate
        if (accepted_times.find(tx1_id) == accepted_times.end() ||
            fee_rates.find(tx1_id) == fee_rates.end()) {
            continue;
        }

        for (const auto& [tx2_id, tx2_order] : selected_order) {
            // Skip if we don't have accepted time or fee rate
            if (accepted_times.find(tx2_id) == accepted_times.end() ||
                fee_rates.find(tx2_id) == fee_rates.end()) {
                continue;
            }

            // Skip same transaction
            if (tx1_id == tx2_id) {
                continue;
            }

            uint64_t tx1_accepted = accepted_times[tx1_id];
            uint64_t tx2_accepted = accepted_times[tx2_id];
            double tx1_fee = fee_rates[tx1_id];
            double tx2_fee = fee_rates[tx2_id];

            // Check if:
            // 1. tx1 arrived before tx2 (tx1_accepted < tx2_accepted)
            // 2. tx1 and tx2 have equal fee rates
            // 3. tx2 was selected before tx1 (tx2_order < tx1_order)
            // This is MEV-like reordering (can't be justified by fees)
            if (tx1_accepted < tx2_accepted &&
                tx1_fee == tx2_fee &&
                tx2_order < tx1_order) {

                EconomicIncentiveViolation violation(
                    getName(),
                    "Equal-fee transactions reordered compared to arrival order",
                    tx1_accepted
                );
                violation.tx_id = tx1_id;
                violation.details = "Transaction " + tx1_id +
                                   " arrived at " + std::to_string(tx1_accepted) +
                                   " but was selected at order " + std::to_string(tx1_order) + "," +
                                   " while transaction " + tx2_id +
                                   " arrived at " + std::to_string(tx2_accepted) +
                                   " but was selected at order " + std::to_string(tx2_order) + "." +
                                   " Both have fee rate " + std::to_string(tx1_fee) + " sat/byte." +
                                   " Reordering suggests MEV extraction.";
                violations.push_back(violation);

                // Only report one violation per transaction pair
                break;
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
