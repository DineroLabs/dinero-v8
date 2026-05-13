#include "economic_liveness_oracle_e7.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicLivenessViolation> E7Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicLivenessViolation> violations;

    // Get all RBF replacement events
    auto rbf_events = getEventsByType(trace, EconomicEventType::TX_REPLACED_RBF);

    for (const auto& event : rbf_events) {
        // Check if replacement was successful
        if (!event.success) {
            EconomicLivenessViolation violation(
                getName(),
                "RBF replacement failed",
                event.timestamp
            );
            violation.tx_id = event.tx_id;
            violation.details = "Replacement transaction rejected: " + event.error_message;

            if (event.replaced_tx_id) {
                violation.details += " (attempted to replace: " + *event.replaced_tx_id + ")";
            }

            violations.push_back(violation);
        }

        // Check that old transaction is not confirmed after replacement
        if (event.success && event.replaced_tx_id) {
            // Check if the OLD transaction was confirmed after replacement
            auto old_tx_confirmed_time = getTxConfirmedTime(trace, *event.replaced_tx_id);
            if (old_tx_confirmed_time && *old_tx_confirmed_time > event.timestamp) {
                EconomicLivenessViolation violation(
                    getName(),
                    "Replaced transaction was confirmed after RBF",
                    *old_tx_confirmed_time
                );
                violation.tx_id = *event.replaced_tx_id;
                violation.details = "Old transaction " + *event.replaced_tx_id +
                                   " confirmed after being replaced by " + *event.tx_id;
                violations.push_back(violation);
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
