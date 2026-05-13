#pragma once

#include "economic_attack_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E16Oracle - Double-Spend Attack Resistance
 *
 * Property: Conflicting transactions don't both confirm (double-spend prevention)
 *
 * Violation conditions:
 * - Two conflicting transactions both confirmed in blockchain
 * - Same input spent in multiple confirmed transactions
 * - Double-spend attack succeeds
 *
 * Observable facts:
 * - TX_INCLUDED_IN_BLOCK events show confirmations
 * - Conflicting transaction IDs (e.g., tx_v1, tx_v2)
 * - Transaction confirmation times and heights
 *
 * Pattern: Check that no two conflicting transactions are both confirmed
 *
 * Note: This is a critical security property. Double-spend attacks undermine
 * the fundamental value proposition of the blockchain.
 */
class E16Oracle : public EconomicAttackOracle {
public:
    std::string getName() const override {
        return "E16: Double-Spend Attack Resistance";
    }

protected:
    std::vector<EconomicAttackViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
