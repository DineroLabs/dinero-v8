#pragma once

#include "economic_incentive_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E15Oracle - Economic DoS Resistance
 *
 * Property: System remains functional under economic DoS attack
 *
 * Violation conditions:
 * - System accepts unbounded free or very-low-fee transactions
 * - Transactions with fees below minimum relay fee are accepted
 * - Economic DoS attack successfully floods the mempool
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events show accepted transactions
 * - Event fee_rate shows transaction fees
 * - EconomicPolicy min_relay_fee_una defines minimum
 * - Event success flag shows acceptance
 *
 * Pattern: Check that all accepted transactions meet minimum fee requirements
 *
 * Note: This checks that the system enforces minimum relay fees to prevent
 * economic DoS attacks. Without minimum fees, attackers can flood the network
 * with free/low-fee transactions.
 *
 * Simplified for Phase 6d: We check if any transaction was accepted with
 * fee rate below the minimum relay fee (calculated from policy).
 */
class E15Oracle : public EconomicIncentiveOracle {
public:
    std::string getName() const override {
        return "E15: Economic DoS Resistance";
    }

protected:
    std::vector<EconomicIncentiveViolation> observeTrace(const EconomicTrace& trace) override;

private:
    // Calculate minimum fee rate from policy (una per byte)
    double calculateMinFeeRate(const EconomicPolicy& policy) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
