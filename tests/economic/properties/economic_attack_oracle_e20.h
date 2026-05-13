#pragma once

#include "economic_attack_oracle.h"

namespace dinero {
namespace economic {
namespace test {

/**
 * E20Oracle - Selfish Mining Resistance
 *
 * Property: Miners don't gain advantage by withholding blocks
 *
 * Violation conditions:
 * - Blocks withheld and then released to cause reorg
 * - Multiple blocks released simultaneously (withholding pattern)
 * - Miner gains unfair advantage through strategic block release
 *
 * Observable facts:
 * - BLOCK_TEMPLATE_ASSEMBLED events show when blocks created
 * - TX_INCLUDED_IN_BLOCK events show when blocks published
 * - Large time gap between creation and publication suggests withholding
 *
 * Pattern: Check for suspicious block release patterns
 *
 * Note: Selfish mining is a strategy where miners withhold blocks to gain
 * advantage over honest miners. This undermines network security and fairness.
 *
 * Simplified for Phase 6e: We check for patterns of multiple blocks
 * being assembled close together (suggesting withholding and batch release).
 */
class E20Oracle : public EconomicAttackOracle {
public:
    std::string getName() const override {
        return "E20: Selfish Mining Resistance";
    }

protected:
    std::vector<EconomicAttackViolation> observeTrace(const EconomicTrace& trace) override;

private:
    // Threshold for detecting batch block release (in timestamp units)
    static constexpr uint64_t BATCH_RELEASE_THRESHOLD = 10;
};

} // namespace test
} // namespace economic
} // namespace dinero
