#include "economic_attack_oracle_e18.h"
#include <map>
#include <set>

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicAttackViolation> E18Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicAttackViolation> violations;

    // Track transactions and look for malleability indicators
    // For this oracle, we check if any transaction appears to have been
    // malleated (indicated by similar tx IDs with different suffixes)

    // Collect all transaction IDs
    std::set<std::string> all_tx_ids;
    for (const auto& event : trace.events) {
        if (event.tx_id) {
            all_tx_ids.insert(*event.tx_id);
        }
    }

    // Check for potential malleability: transactions with "_mal" suffix
    // or patterns like "tx1_orig" and "tx1_malleated"
    for (const auto& tx_id : all_tx_ids) {
        // Check if this transaction ID indicates malleability
        if (tx_id.find("_mal") != std::string::npos ||
            tx_id.find("malleated") != std::string::npos) {

            // This transaction ID suggests malleability occurred
            EconomicAttackViolation violation(
                getName(),
                "Transaction malleability detected",
                0  // Timestamp not critical for malleability detection
            );
            violation.tx_id = tx_id;
            violation.details = "Transaction ID \"" + tx_id + "\" suggests malleability attack." +
                               " Transaction IDs should be immutable.";

            // Find when this transaction appeared
            for (const auto& event : trace.events) {
                if (event.tx_id && *event.tx_id == tx_id) {
                    violation.timestamp = event.timestamp;
                    violation.involved_nodes.push_back(event.node_id);
                    break;
                }
            }

            violations.push_back(violation);
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
