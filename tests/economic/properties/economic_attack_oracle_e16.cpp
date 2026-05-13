#include "economic_attack_oracle_e16.h"

namespace dinero {
namespace economic {
namespace test {

std::vector<EconomicAttackViolation> E16Oracle::observeTrace(const EconomicTrace& trace) {
    std::vector<EconomicAttackViolation> violations;

    // Get all confirmed transactions
    auto confirmed_txs = getConfirmedTxs(trace);

    // Check for conflicting transactions both being confirmed
    for (size_t i = 0; i < confirmed_txs.size(); ++i) {
        for (size_t j = i + 1; j < confirmed_txs.size(); ++j) {
            const std::string& tx1_id = confirmed_txs[i];
            const std::string& tx2_id = confirmed_txs[j];

            // Check if these transactions are conflicting (double-spend)
            if (areConflicting(tx1_id, tx2_id)) {
                auto tx1_time = getTxConfirmedTime(trace, tx1_id);
                auto tx2_time = getTxConfirmedTime(trace, tx2_id);
                auto tx1_height = getTxConfirmedHeight(trace, tx1_id);
                auto tx2_height = getTxConfirmedHeight(trace, tx2_id);

                EconomicAttackViolation violation(
                    getName(),
                    "Conflicting transactions both confirmed (double-spend)",
                    tx2_time.value_or(0)
                );
                violation.tx_id = tx2_id;
                violation.details = "Conflicting transactions " + tx1_id + " and " + tx2_id +
                                   " both confirmed.";

                if (tx1_time && tx2_time) {
                    violation.details += " Confirmed at timestamps " +
                                       std::to_string(*tx1_time) + " and " +
                                       std::to_string(*tx2_time) + ".";
                }

                if (tx1_height && tx2_height) {
                    violation.details += " At heights " +
                                       std::to_string(*tx1_height) + " and " +
                                       std::to_string(*tx2_height) + ".";
                }

                violation.details += " Double-spend attack succeeded.";

                violations.push_back(violation);
            }
        }
    }

    return violations;
}

} // namespace test
} // namespace economic
} // namespace dinero
