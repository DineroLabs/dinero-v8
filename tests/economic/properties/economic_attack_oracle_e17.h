#pragma once

#include "economic_attack_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E17Oracle - Fee Sniping Resistance
 *
 * Property: Miners don't reorg recent blocks to steal fees
 *
 * Violation conditions:
 * - Transaction confirmed, then reorged out, then confirmed again
 * - Reorg appears to be motivated by fee collection (high-fee tx)
 * - Frequent shallow reorgs suggest fee sniping behavior
 *
 * Observable facts:
 * - TX_INCLUDED_IN_BLOCK events show initial confirmation
 * - TX_REORGED_OUT events show reorg victims
 * - TX_INCLUDED_IN_BLOCK events (again) show re-confirmation
 * - Block heights show reorg depth
 *
 * Pattern: Check for suspicious reorg patterns (tx confirmed → reorged → re-confirmed)
 *
 * Note: Fee sniping undermines blockchain stability and user confidence.
 * Shallow reorgs to capture high fees are economically rational but harmful.
 */
class E17Oracle : public EconomicAttackOracle {
public:
    std::string getName() const override {
        return "E17: Fee Sniping Resistance";
    }

protected:
    std::vector<EconomicAttackViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
