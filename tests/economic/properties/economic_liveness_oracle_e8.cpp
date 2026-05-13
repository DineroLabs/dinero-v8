#include "economic_liveness_oracle_e8.h"
#include <cmath>

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicLivenessViolation> E8Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicLivenessViolation> violations;

    // Get all fee estimate update events
    auto estimate_events = getEventsByType(trace, EconomicEventType::FEE_ESTIMATE_UPDATED);

    for (const auto& event : estimate_events) {
        if (!event.estimated_fee_rate) {
            continue;
        }

        double fee_rate = *event.estimated_fee_rate;

        // Check for invalid values (NaN, infinity)
        if (std::isnan(fee_rate) || std::isinf(fee_rate)) {
            EconomicLivenessViolation violation(
                getName(),
                "Fee estimate is invalid (NaN or infinite)",
                event.timestamp
            );
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee rate: " + std::to_string(fee_rate);
            violations.push_back(violation);
            continue;
        }

        // Check for negative fee rate
        if (fee_rate < 0.0) {
            EconomicLivenessViolation violation(
                getName(),
                "Fee estimate is negative",
                event.timestamp
            );
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee rate: " + std::to_string(fee_rate) + " sat/byte";
            violations.push_back(violation);
            continue;
        }

        // Check for unreasonably high fee rate
        if (fee_rate > MAX_REASONABLE_FEE_RATE) {
            EconomicLivenessViolation violation(
                getName(),
                "Fee estimate unreasonably high",
                event.timestamp
            );
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee rate: " + std::to_string(fee_rate) +
                               " sat/byte (max reasonable: " +
                               std::to_string(MAX_REASONABLE_FEE_RATE) + ")";
            violations.push_back(violation);
            continue;
        }

        // Check for fee rate below minimum relay fee (if available)
        double min_relay_rate = static_cast<double>(trace.policy.min_relay_fee_una) / 1000.0;  // Assume 1KB tx
        if (fee_rate < min_relay_rate) {
            EconomicLivenessViolation violation(
                getName(),
                "Fee estimate below minimum relay fee",
                event.timestamp
            );
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Fee rate: " + std::to_string(fee_rate) +
                               " sat/byte, Min relay: " + std::to_string(min_relay_rate);
            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
