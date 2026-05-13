#include "economic_safety_oracle_e2.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicViolation> E2Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicViolation> violations;

    // Check all confirmed transactions
    auto confirmed_events = getEventsByType(trace, EconomicEventType::TX_INCLUDED_IN_BLOCK);

    for (const auto& event : confirmed_events) {
        // Skip if missing required data
        if (!event.tx_id || !event.fee_una || !event.input_value || !event.output_value) {
            continue;
        }

        uint64_t input_value = *event.input_value;
        uint64_t output_value = *event.output_value;
        uint64_t fee_una = *event.fee_una;

        // Check value conservation: input_value = output_value + fee_una
        if (output_value > input_value) {
            EconomicViolation violation(
                getName(),
                "Outputs exceed inputs (negative fee)",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Inputs: " + std::to_string(input_value) +
                               ", Outputs: " + std::to_string(output_value);
            violations.push_back(violation);
            continue;
        }

        // Check fee calculation
        uint64_t calculated_fee = input_value - output_value;
        if (calculated_fee != fee_una) {
            EconomicViolation violation(
                getName(),
                "Fee mismatch in confirmed transaction",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Expected fee: " + std::to_string(calculated_fee) +
                               ", Actual fee: " + std::to_string(fee_una);
            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
