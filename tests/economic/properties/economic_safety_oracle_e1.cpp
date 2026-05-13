#include "economic_safety_oracle_e1.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicViolation> E1Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicViolation> violations;

    // Check all TX_ACCEPTED_TO_MEMPOOL events
    auto accepted_events = getEventsByType(trace, EconomicEventType::TX_ACCEPTED_TO_MEMPOOL);

    for (const auto& event : accepted_events) {
        // Skip if missing required data
        if (!event.tx_id || !event.fee_una || !event.input_value || !event.output_value) {
            continue;
        }

        // Validate fee
        auto error = validateFee(*event.fee_una, *event.input_value, *event.output_value);
        if (error) {
            EconomicViolation violation(
                getName(),
                "Invalid fee transaction accepted: " + *error,
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee: " + std::to_string(*event.fee_una) +
                               ", Inputs: " + std::to_string(*event.input_value) +
                               ", Outputs: " + std::to_string(*event.output_value);
            violations.push_back(violation);
        }
    }

    return violations;
}

std::optional<std::string> E1Oracle::validateFee(
    uint64_t fee_una,
    uint64_t input_value,
    uint64_t output_value
) const {
    // Check value conservation: outputs must not exceed inputs
    if (output_value > input_value) {
        return "Outputs exceed inputs";
    }

    // Calculate expected fee
    uint64_t expected_fee = input_value - output_value;

    // Check fee matches calculation
    if (fee_una != expected_fee) {
        return "Fee mismatch (expected " + std::to_string(expected_fee) +
               ", got " + std::to_string(fee_una) + ")";
    }

    return std::nullopt;  // Valid
}

} // namespace test
} // namespace economic
} // namespace dinero
