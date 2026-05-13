#pragma once

#include "economic_incentive_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E12Oracle - Fee Market Efficiency
 *
 * Property: Fee market clears efficiently (high-fee txs confirm faster)
 *
 * Violation conditions:
 * - Low-fee transaction confirms before high-fee transaction
 * - Transaction with higher fee rate waits longer than lower-fee transaction
 * - Fee market fails to prioritize economic value
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events show when txs enter mempool
 * - TX_INCLUDED_IN_BLOCK events show confirmation time
 * - Event fee_rate shows transaction priority
 * - Event timestamp shows confirmation order
 *
 * Pattern: Check that higher-fee transactions confirm before lower-fee transactions
 *
 * Note: This checks fee market efficiency from user perspective.
 * Users expect: "If I pay more, my tx confirms faster"
 */
class E12Oracle : public EconomicIncentiveOracle {
public:
    std::string getName() const override {
        return "E12: Fee Market Efficiency";
    }

protected:
    std::vector<EconomicIncentiveViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
