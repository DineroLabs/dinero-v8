#pragma once

#include "economic_safety_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E5Oracle - Dust Threshold
 *
 * Property: Transactions with dust outputs are properly rejected
 *
 * Violation conditions:
 * - Transaction accepted/confirmed with output value < dust_threshold
 *
 * Observable facts:
 * - Policy has dust_threshold_una
 * - TX_REJECTED_DUST events show dust rejections
 * - TX_ACCEPTED_TO_MEMPOOL events should not have dust outputs
 * - For simplification: Check if output_value is suspiciously low (< dust threshold)
 *
 * Pattern: Check rejected txs for dust, ensure no accepted txs have dust
 *
 * Note: Full dust checking requires individual output inspection.
 * For Phase 6b, we use a simplified check: if output_value < dust_threshold,
 * assume dust (since output_value is total of all outputs).
 */
class E5Oracle : public EconomicSafetyOracle {
public:
    std::string getName() const override {
        return "E5: Dust Threshold";
    }

protected:
    std::vector<EconomicViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
