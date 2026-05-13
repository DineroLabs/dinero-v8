#pragma once

#include "economic_safety_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E3Oracle - Fee Overflow Protection
 *
 * Property: Fee calculations never overflow
 *
 * Violation conditions:
 * - Transaction accepted/confirmed with unreasonably high fee (> max reasonable)
 * - Transaction accepted where fee calculation could overflow uint64_t
 *
 * Observable facts:
 * - Maximum reasonable fee: 1 BTC = 100,000,000 una
 * - Any fee > 100M sats is unreasonable (likely overflow)
 * - Check both accepted and confirmed transactions
 *
 * Pattern: Check all accepted/confirmed txs for unreasonable fees
 */
class E3Oracle : public EconomicSafetyOracle {
public:
    std::string getName() const override {
        return "E3: Fee Overflow Protection";
    }

protected:
    std::vector<EconomicViolation> observeTrace(const EconomicTrace& trace) override;

private:
    // Maximum reasonable fee: 1 BTC = 100,000,000 una
    static constexpr uint64_t MAX_REASONABLE_FEE = 100000000;
};

} // namespace test
} // namespace economic
} // namespace dinero
