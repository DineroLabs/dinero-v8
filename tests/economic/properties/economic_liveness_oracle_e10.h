#pragma once

#include "economic_liveness_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E10Oracle - Economic Finality
 *
 * Property: Transactions with sufficient fee depth don't reorg out
 *
 * Violation conditions:
 * - Transaction confirmed, then later TX_REORGED_OUT event occurs
 * - Transaction appears in confirmed block but later disappears
 *
 * Observable facts:
 * - TX_INCLUDED_IN_BLOCK events show confirmations
 * - TX_REORGED_OUT events show reorg victims
 * - Same tx_id should not have both events
 *
 * Pattern: Check that confirmed txs don't later reorg out
 *
 * Note: For Phase 6c, we check basic invariant: confirmed tx should not reorg.
 * Full economic finality (with fee depth) requires deeper analysis.
 */
class E10Oracle : public EconomicLivenessOracle {
public:
    std::string getName() const override {
        return "E10: Economic Finality";
    }

protected:
    std::vector<EconomicLivenessViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
