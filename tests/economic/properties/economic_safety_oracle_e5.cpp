#include "economic_safety_oracle_e5.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicViolation> E5Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicViolation> violations;

    uint64_t dust_threshold = trace.policy.dust_threshold_una;

    // Check TX_ACCEPTED_TO_MEMPOOL events
    // Simplified check: if total output_value < dust_threshold, likely has dust output
    auto accepted_events = getEventsByType(trace, EconomicEventType::TX_ACCEPTED_TO_MEMPOOL);
    for (const auto& event : accepted_events) {
        if (!event.output_value || !event.tx_id) {
            continue;
        }

        // If total outputs < dust threshold, definitely has dust
        if (*event.output_value < dust_threshold) {
            EconomicViolation violation(
                getName(),
                "Transaction accepted with dust output (total outputs < dust threshold)",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Total outputs: " + std::to_string(*event.output_value) +
                               " sats, Dust threshold: " + std::to_string(dust_threshold) + " sats";
            violations.push_back(violation);
        }
    }

    // Check TX_INCLUDED_IN_BLOCK events
    auto confirmed_events = getEventsByType(trace, EconomicEventType::TX_INCLUDED_IN_BLOCK);
    for (const auto& event : confirmed_events) {
        if (!event.output_value || !event.tx_id) {
            continue;
        }

        // If total outputs < dust threshold, definitely has dust
        if (*event.output_value < dust_threshold) {
            EconomicViolation violation(
                getName(),
                "Transaction confirmed with dust output (total outputs < dust threshold)",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Total outputs: " + std::to_string(*event.output_value) +
                               " sats, Dust threshold: " + std::to_string(dust_threshold) + " sats";
            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
