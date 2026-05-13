#pragma once

#include "economic_safety_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E2Oracle - Value Conservation
 *
 * Property: Transaction values are properly conserved (input - output = fee ≥ 0)
 *
 * Violation conditions:
 * - Transaction confirmed where outputs > inputs (negative fee)
 * - Transaction confirmed where (input - output) != fee
 *
 * Observable facts:
 * - TX_INCLUDED_IN_BLOCK events show confirmed transactions
 * - Each event has input_value, output_value, fee_una
 * - Value conservation: input_value = output_value + fee_una
 *
 * Pattern: Check all confirmed transactions for value conservation
 */
class E2Oracle : public EconomicSafetyOracle {
public:
    std::string getName() const override {
        return "E2: Value Conservation";
    }

protected:
    std::vector<EconomicViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
