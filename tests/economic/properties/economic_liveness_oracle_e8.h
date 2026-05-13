#pragma once

#include "economic_liveness_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E8Oracle - Fee Estimation
 *
 * Property: Fee estimator provides bounded estimates (not infinite/NaN)
 *
 * Violation conditions:
 * - FEE_ESTIMATE_UPDATED event with invalid estimate (NaN, infinite, negative)
 * - Fee estimate unreasonably high (> max reasonable fee)
 * - Fee estimate unreasonably low (< min relay fee)
 *
 * Observable facts:
 * - FEE_ESTIMATE_UPDATED events show estimator updates
 * - Event contains estimated_fee_rate
 * - Estimate should be bounded and reasonable
 *
 * Pattern: Check that all fee estimates are valid and bounded
 */
class E8Oracle : public EconomicLivenessOracle {
public:
    std::string getName() const override {
        return "E8: Fee Estimation";
    }

protected:
    std::vector<EconomicLivenessViolation> observeTrace(const EconomicTrace& trace) override;

private:
    // Maximum reasonable fee rate: 1000 sat/byte (very high but not overflow)
    static constexpr double MAX_REASONABLE_FEE_RATE = 1000.0;
};

} // namespace test
} // namespace economic
} // namespace dinero
