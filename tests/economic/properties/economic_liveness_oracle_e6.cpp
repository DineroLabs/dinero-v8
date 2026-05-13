#include "economic_liveness_oracle_e6.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicLivenessViolation> E6Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicLivenessViolation> violations;

    // Check if any blocks were mined
    auto template_events = getEventsByType(trace, EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED);
    if (template_events.empty()) {
        // No blocks mined, so no violation (liveness requires progress)
        return violations;
    }

    // Get unconfirmed transactions
    auto unconfirmed_txs = getUnconfirmedTxs(trace);

    for (const auto& tx_id : unconfirmed_txs) {
        EconomicLivenessViolation violation(
            getName(),
            "Valid fee-bearing transaction accepted but never confirmed",
            trace.end_time
        );
        violation.tx_id = tx_id;
        violation.details = "Transaction remained in mempool until end of trace";

        // Get accepted time for additional context
        auto accepted_time = getTxAcceptedTime(trace, tx_id);
        if (accepted_time) {
            uint64_t wait_time = trace.end_time - *accepted_time;
            violation.details += " (waited " + std::to_string(wait_time) + " ms)";
        }

        violations.push_back(violation);
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
