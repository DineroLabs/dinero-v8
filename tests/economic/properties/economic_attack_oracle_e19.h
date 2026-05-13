#pragma once

#include "economic_attack_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E19Oracle - Time-Warp Attack Resistance
 *
 * Property: Block timestamps are valid and monotonically increasing
 *
 * Violation conditions:
 * - Block timestamp goes backwards (non-monotonic)
 * - Block timestamp far in future or past (invalid)
 * - Time-warp attack manipulates difficulty by timestamp manipulation
 *
 * Observable facts:
 * - BLOCK_TEMPLATE_ASSEMBLED events show block creation times
 * - Event timestamps show when blocks were created
 * - Block heights show sequence
 *
 * Pattern: Check that block assembly timestamps are monotonically increasing
 *
 * Note: Time-warp attacks manipulate block timestamps to reduce difficulty
 * and mine blocks faster. This undermines the security model.
 *
 * Simplified for Phase 6e: We check that block template assembly timestamps
 * are monotonically increasing (no backwards time jumps).
 */
class E19Oracle : public EconomicAttackOracle {
public:
    std::string getName() const override {
        return "E19: Time-Warp Attack Resistance";
    }

protected:
    std::vector<EconomicAttackViolation> observeTrace(const EconomicTrace& trace) override;
};

} // namespace test
} // namespace economic
} // namespace dinero
