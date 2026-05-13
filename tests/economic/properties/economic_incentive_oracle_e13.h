#pragma once

#include "economic_incentive_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E13Oracle - MEV Resistance
 *
 * Property: No transaction reordering for MEV (Miner Extractable Value) extraction
 *
 * Violation conditions:
 * - Transactions reordered in block compared to mempool arrival order
 * - Reordering cannot be justified by fee differences
 * - Same-fee transactions appear out-of-order in block
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events show arrival order
 * - TX_SELECTED_FOR_BLOCK events show block inclusion order
 * - Event fee_rate distinguishes fee-justified reordering
 * - Event timestamp shows arrival order
 *
 * Pattern: Check that transactions with equal fees maintain arrival order
 *
 * Note: We allow reordering by fee (higher fee first), but not reordering
 * of equal-fee transactions (which suggests MEV extraction).
 *
 * Simplified for Phase 6d: We check if any two transactions with equal fee rates
 * are reordered compared to their mempool arrival order.
 */
class E13Oracle : public EconomicIncentiveOracle {
public:
    std::string getName() const override {
        return "E13: MEV Resistance";
    }

protected:
    std::vector<EconomicIncentiveViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
