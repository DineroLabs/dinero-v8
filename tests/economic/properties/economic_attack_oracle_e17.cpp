#include "economic_attack_oracle_e17.h"
#include <map>

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicAttackViolation> E17Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicAttackViolation> violations;

    // Track transaction lifecycle: confirmed -> reorged -> re-confirmed
    std::map<std::string, std::vector<uint64_t>> confirmation_times;
    std::map<std::string, std::vector<uint64_t>> reorg_times;

    for (const auto& event : trace.events) {
        if (!event.tx_id) continue;

        if (event.type == EconomicEventType::TX_INCLUDED_IN_BLOCK) {
            confirmation_times[*event.tx_id].push_back(event.timestamp);
        } else if (event.type == EconomicEventType::TX_REORGED_OUT) {
            reorg_times[*event.tx_id].push_back(event.timestamp);
        }
    }

    // Check for suspicious patterns: confirmed -> reorged -> re-confirmed
    for (const auto& [tx_id, confirms] : confirmation_times) {
        if (confirms.size() < 2) continue;  // Need at least 2 confirmations

        // Check if there's a reorg between confirmations
        if (reorg_times.find(tx_id) != reorg_times.end()) {
            const auto& reorgs = reorg_times[tx_id];

            // Check if reorg happened between first and second confirmation
            if (!reorgs.empty()) {
                uint64_t first_confirm = confirms[0];
                uint64_t second_confirm = confirms.size() > 1 ? confirms[1] : confirms[0];
                uint64_t reorg_time = reorgs[0];

                // Pattern: confirmed -> reorged -> re-confirmed (fee sniping)
                if (first_confirm < reorg_time && reorg_time < second_confirm) {
                    EconomicAttackViolation violation(
                        getName(),
                        "Transaction reorged and re-confirmed (fee sniping pattern)",
                        reorg_time
                    );
                    violation.tx_id = tx_id;
                    violation.details = "Transaction " + tx_id +
                                       " confirmed at " + std::to_string(first_confirm) + "," +
                                       " reorged out at " + std::to_string(reorg_time) + "," +
                                       " re-confirmed at " + std::to_string(second_confirm) + "." +
                                       " This pattern suggests fee sniping attack.";
                    violations.push_back(violation);
                }
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
