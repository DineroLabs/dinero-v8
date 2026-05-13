#include "economic_safety_oracle_e3.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicViolation> E3Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicViolation> violations;

    // Check TX_ACCEPTED_TO_MEMPOOL events
    auto accepted_events = getEventsByType(trace, EconomicEventType::TX_ACCEPTED_TO_MEMPOOL);
    for (const auto& event : accepted_events) {
        if (event.fee_una && *event.fee_una > MAX_REASONABLE_FEE) {
            EconomicViolation violation(
                getName(),
                "Transaction accepted with unreasonably high fee (possible overflow)",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee: " + std::to_string(*event.fee_una) +
                               " sats (max reasonable: " + std::to_string(MAX_REASONABLE_FEE) + ")";
            violations.push_back(violation);
        }
    }

    // Check TX_INCLUDED_IN_BLOCK events
    auto confirmed_events = getEventsByType(trace, EconomicEventType::TX_INCLUDED_IN_BLOCK);
    for (const auto& event : confirmed_events) {
        if (event.fee_una && *event.fee_una > MAX_REASONABLE_FEE) {
            EconomicViolation violation(
                getName(),
                "Transaction confirmed with unreasonably high fee (possible overflow)",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee: " + std::to_string(*event.fee_una) +
                               " sats (max reasonable: " + std::to_string(MAX_REASONABLE_FEE) + ")";
            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
