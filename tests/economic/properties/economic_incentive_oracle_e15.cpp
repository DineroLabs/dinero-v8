#include "economic_incentive_oracle_e15.h"

namespace dinero {
namespace economic {
namespace test {

double E15Oracle::calculateMinFeeRate(const EconomicPolicy& policy) const {
    // Minimum relay fee is typically expressed as una per kilobyte
    // Convert to una per byte for comparison
    // Assuming min_relay_fee_una is per kilobyte (1000 bytes)
    // Standard Bitcoin minimum is 1000 una/KB = 1 una/byte
    return policy.min_relay_fee_una / 1000.0;
}

std::vector<EconomicIncentiveViolation> E15Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicIncentiveViolation> violations;

    // Calculate minimum fee rate from policy
    double min_fee_rate = calculateMinFeeRate(trace.policy);

    // Get all accepted transactions
    auto accepted_events = getEventsByType(trace, EconomicEventType::TX_ACCEPTED_TO_MEMPOOL);

    // Check if any accepted transaction has fee rate below minimum
    for (const auto& event : accepted_events) {
        if (!event.success || !event.tx_id || !event.fee_rate) {
            continue;
        }

        double tx_fee_rate = *event.fee_rate;

        // Check if transaction fee rate is below minimum relay fee
        // Allow small epsilon for floating point comparison
        const double epsilon = 0.0001;
        if (tx_fee_rate < min_fee_rate - epsilon) {
            EconomicIncentiveViolation violation(
                getName(),
                "Transaction accepted with fee below minimum relay fee",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Transaction " + *event.tx_id +
                               " accepted with fee rate " + std::to_string(tx_fee_rate) + " sat/byte," +
                               " which is below minimum relay fee " + std::to_string(min_fee_rate) + " sat/byte." +
                               " System is vulnerable to economic DoS attack.";
            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
