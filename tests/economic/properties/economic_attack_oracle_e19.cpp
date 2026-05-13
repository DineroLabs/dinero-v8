#include "economic_attack_oracle_e19.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicAttackViolation> E19Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicAttackViolation> violations;

    // Get all block template assembly events (block creation)
    auto block_events = getBlockTemplateEvents(trace);

    // Check that block timestamps are monotonically increasing
    uint64_t prev_timestamp = 0;
    for (const auto& event : block_events) {
        uint64_t current_timestamp = event.timestamp;

        // Check for backwards time jump
        if (prev_timestamp > 0 && current_timestamp < prev_timestamp) {
            EconomicAttackViolation violation(
                getName(),
                "Block timestamp goes backwards (time-warp attack)",
                current_timestamp
            );
            violation.involved_nodes.push_back(event.node_id);
            violation.details = "Block assembled at timestamp " + std::to_string(current_timestamp) +
                               " which is before previous block timestamp " + std::to_string(prev_timestamp) + "." +
                               " This indicates time-warp attack or clock manipulation.";
            violations.push_back(violation);
        }

        prev_timestamp = current_timestamp;
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
