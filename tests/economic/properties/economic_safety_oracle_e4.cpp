#include "economic_safety_oracle_e4.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicViolation> E4Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicViolation> violations;

    uint64_t min_relay_fee = trace.policy.min_relay_fee_una;

    // Check TX_ACCEPTED_TO_MEMPOOL events
    auto accepted_events = getEventsByType(trace, EconomicEventType::TX_ACCEPTED_TO_MEMPOOL);
    for (const auto& event : accepted_events) {
        if (!event.fee_una || !event.tx_id) {
            continue;
        }

        if (*event.fee_una < min_relay_fee) {
            EconomicViolation violation(
                getName(),
                "Transaction accepted with fee below minimum relay fee",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee: " + std::to_string(*event.fee_una) +
                               " sats, Min relay fee: " + std::to_string(min_relay_fee) + " sats";
            violations.push_back(violation);
        }
    }

    // Check TX_RELAYED events (if any)
    auto relayed_events = getEventsByType(trace, EconomicEventType::TX_RELAYED);
    for (const auto& event : relayed_events) {
        if (!event.fee_una || !event.tx_id) {
            continue;
        }

        if (*event.fee_una < min_relay_fee) {
            EconomicViolation violation(
                getName(),
                "Transaction relayed with fee below minimum relay fee",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee: " + std::to_string(*event.fee_una) +
                               " sats, Min relay fee: " + std::to_string(min_relay_fee) + " sats";
            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
