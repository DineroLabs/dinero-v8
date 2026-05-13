#pragma once

#include "economic_safety_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E4Oracle - Minimum Relay Fee
 *
 * Property: Transactions below minimum relay fee are never accepted
 *
 * Violation conditions:
 * - Transaction accepted to mempool with fee < min_relay_fee
 * - Transaction relayed (TX_RELAYED event) with fee < min_relay_fee
 *
 * Observable facts:
 * - Policy has min_relay_fee_una
 * - TX_ACCEPTED_TO_MEMPOOL events show accepted transactions
 * - TX_RELAYED events show relayed transactions
 * - Fee must be >= min_relay_fee
 *
 * Pattern: Check all accepted/relayed txs against policy minimum
 */
class E4Oracle : public EconomicSafetyOracle {
public:
    std::string getName() const override {
        return "E4: Minimum Relay Fee";
    }

protected:
    std::vector<EconomicViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
