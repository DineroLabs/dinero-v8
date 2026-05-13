#pragma once

#include "economic_safety_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E1Oracle - Fee Validation
 *
 * Property: Invalid fee transactions are never accepted
 *
 * Violation conditions:
 * - Transaction accepted with fee < 0 (outputs > inputs)
 * - Transaction accepted with fee calculation error
 * - Transaction accepted where fee != (inputs - outputs)
 *
 * Observable facts:
 * - TX_ACCEPTED_TO_MEMPOOL events have fee_una, input_value, output_value
 * - Fee must equal (input_value - output_value)
 * - Fee must be non-negative
 *
 * Pattern: Check all TX_ACCEPTED events for invalid fees
 */
class E1Oracle : public EconomicSafetyOracle {
public:
    std::string getName() const override {
        return "E1: Fee Validation";
    }

protected:
    std::vector<EconomicViolation> observeTrace(const EconomicTrace& trace) override;

private:
    /**
     * Check if fee is valid for transaction
     *
     * @return nullopt if valid, error description if invalid
     */
    std::optional<std::string> validateFee(
        uint64_t fee_una,
        uint64_t input_value,
        uint64_t output_value
    ) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
