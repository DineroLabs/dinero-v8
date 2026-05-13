#pragma once

#include "economic_liveness_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E7Oracle - Mempool Replacement (RBF)
 *
 * Property: RBF transactions eventually replace lower-fee versions
 *
 * Violation conditions:
 * - TX_REPLACED_RBF event does not occur for valid RBF attempt
 * - Higher-fee replacement submitted but original transaction remains
 *
 * Observable facts:
 * - TX_REPLACED_RBF events show successful replacements
 * - Event contains replaced_tx_id and new tx_id
 * - Both old and new txs should be trackable
 *
 * Pattern: Check that RBF events occur when expected
 *
 * Note: For Phase 6c, we check that if a TX_REPLACED_RBF event occurs,
 * it was successful. Full RBF testing requires action-based scenarios.
 */
class E7Oracle : public EconomicLivenessOracle {
public:
    std::string getName() const override {
        return "E7: Mempool Replacement (RBF)";
    }

protected:
    std::vector<EconomicLivenessViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
