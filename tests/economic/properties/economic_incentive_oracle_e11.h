#pragma once

#include "economic_incentive_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E11Oracle - Mining Incentive Compatibility
 *
 * Property: Miners are incentivized to include highest-fee transactions
 *
 * Violation conditions:
 * - Block template excludes high-fee transaction while including low-fee transaction
 * - Available block space exists but high-fee transactions are excluded
 * - Mining reward is suboptimal compared to available transactions
 *
 * Observable facts:
 * - BLOCK_TEMPLATE_ASSEMBLED events show template creation
 * - TX_SELECTED_FOR_BLOCK events show included transactions
 * - TX_EXCLUDED_FROM_BLOCK events show excluded transactions
 * - Event fee_rate shows transaction priority
 *
 * Pattern: Check that block templates maximize fee revenue
 *
 * Note: This is similar to E9 (Block Assembly) but focuses on miner incentives
 * rather than protocol correctness. E9 checks "does block assembly work correctly",
 * E11 checks "are miners incentivized to maximize fees".
 */
class E11Oracle : public EconomicIncentiveOracle {
public:
    std::string getName() const override {
        return "E11: Mining Incentive Compatibility";
    }

protected:
    std::vector<EconomicIncentiveViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
