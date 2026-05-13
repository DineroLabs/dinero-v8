#include "economic_attack_oracle_e20.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicAttackViolation> E20Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicAttackViolation> violations;

    // Get all block template assembly events
    auto block_events = getBlockTemplateEvents(trace);

    // Check for batch block releases (selfish mining pattern)
    // If multiple blocks are assembled very close together in time,
    // this suggests they were withheld and then released as a batch

    for (size_t i = 1; i < block_events.size(); ++i) {
        uint64_t prev_time = block_events[i-1].timestamp;
        uint64_t curr_time = block_events[i].timestamp;

        // Check if blocks were released very close together
        if (curr_time - prev_time < BATCH_RELEASE_THRESHOLD) {
            // Check if there are more blocks in quick succession
            size_t batch_count = 2;  // Current and previous
            for (size_t j = i + 1; j < block_events.size(); ++j) {
                if (block_events[j].timestamp - curr_time < BATCH_RELEASE_THRESHOLD) {
                    batch_count++;
                    i = j;  // Skip ahead
                } else {
                    break;
                }
            }

            // If we found 2+ blocks released in quick succession, flag as suspicious
            if (batch_count >= 2) {
                EconomicAttackViolation violation(
                    getName(),
                    "Batch block release detected (selfish mining pattern)",
                    curr_time
                );
                violation.involved_nodes.push_back(block_events[i].node_id);
                violation.details = std::to_string(batch_count) + " blocks assembled within " +
                                   std::to_string(BATCH_RELEASE_THRESHOLD) + " time units." +
                                   " This pattern suggests selfish mining (withholding and batch release).";
                violations.push_back(violation);
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
