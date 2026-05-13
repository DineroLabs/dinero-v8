#pragma once

#include "economic_liveness_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E6Oracle - Fee-Bearing TX Inclusion
 *
 * Property: Valid fee-bearing transactions are eventually included in blocks
 *
 * Violation conditions:
 * - Transaction accepted to mempool but never included in block (within timeout)
 * - Valid transaction with sufficient fee remains unconfirmed
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events show accepted transactions
 * - TX_INCLUDED_IN_BLOCK events show confirmed transactions
 * - If tx accepted but not confirmed by end of trace, likely violation
 *
 * Pattern: Check that all accepted txs are eventually confirmed
 *
 * Note: "Eventually" means within the trace duration. For testing,
 * we accept that some txs may remain unconfirmed if trace is short.
 * Violation only if accepted txs AND blocks were mined AND tx not confirmed.
 */
class E6Oracle : public EconomicLivenessOracle {
public:
    std::string getName() const override {
        return "E6: Fee-Bearing TX Inclusion";
    }

protected:
    std::vector<EconomicLivenessViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
