#pragma once

#include "economic_liveness_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E9Oracle - Block Assembly
 *
 * Property: Block templates include highest-fee valid transactions
 *
 * Violation conditions:
 * - Lower-fee transaction included while higher-fee transaction excluded
 * - Block template has space but valid high-fee txs not included
 *
 * Observable facts:
 * - BLOCK_TEMPLATE_ASSEMBLED events show template creation
 * - TX_SELECTED_FOR_BLOCK events show included transactions
 * - TX_EXCLUDED_FROM_BLOCK events show excluded transactions
 * - Event fee_rate shows transaction priority
 *
 * Pattern: Check that excluded txs have lower fee rates than included txs
 */
class E9Oracle : public EconomicLivenessOracle {
public:
    std::string getName() const override {
        return "E9: Block Assembly";
    }

protected:
    std::vector<EconomicLivenessViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
