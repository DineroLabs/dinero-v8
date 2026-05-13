#pragma once

#include "consensus/subsidy.h"
#include <cstdint>
#include <string>

namespace dinero {

/**
 * Supply Validator
 *
 * Provides runtime verification of Dinero's monetary policy.
 * No hard cap — disinflationary with 1 DIN/block tail emission.
 *
 * Key invariants enforced:
 * 1. Supply calculation is monotonic (never decreases)
 * 2. Tail emission floor: subsidy >= 1 DIN for all heights >= 1
 * 3. Halving schedule is correct until tail kicks in
 */
class SupplyValidator {
public:
    /**
     * Verify supply is monotonic (never decreases)
     *
     * @param height1 Earlier height
     * @param height2 Later height
     * @return true if supply at height2 >= supply at height1
     */
    static bool VerifyMonotonicSupply(uint32_t height1, uint32_t height2) {
        if (height2 < height1) return false;
        uint64_t supply1 = ConsensusSubsidy::GetTotalIssuedAtHeight(height1);
        uint64_t supply2 = ConsensusSubsidy::GetTotalIssuedAtHeight(height2);
        return supply2 >= supply1;
    }

    /**
     * Verify tail emission floor is enforced
     *
     * @param height Block height (>= 1)
     * @return true if subsidy >= 1 DIN
     */
    static bool VerifyTailEmissionFloor(uint32_t height) {
        if (height == 0) return true;  // Genesis has no subsidy
        AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);
        return subsidy.GetUna() >= ConsensusSubsidy::TAIL_EMISSION_UNA;
    }

    /**
     * Verify supply at all critical heights
     *
     * Tests supply invariants at key blockchain milestones:
     * - Genesis (height 0): 0 DIN
     * - First PoW block (height 1): 100 DIN
     * - All halving boundaries
     * - Tail emission onset (~height 9,198,001)
     * - Deep tail emission
     *
     * @return true if all invariants hold
     */
    static bool VerifyAllCriticalHeights() {
        // Genesis: 0 DIN
        if (ConsensusSubsidy::GetBlockSubsidy(0) != AmountUna::Zero()) return false;

        // Height 1: 100 DIN (first PoW block)
        if (ConsensusSubsidy::GetBlockSubsidy(1).GetUna() != ConsensusSubsidy::INITIAL_SUBSIDY) return false;

        // All halving boundaries
        for (uint32_t epoch = 0; epoch < 10; epoch++) {
            uint32_t halving_height = 1 + (epoch * ConsensusSubsidy::HALVING_INTERVAL);
            if (!VerifyTailEmissionFloor(halving_height)) return false;
            if (!VerifyMonotonicSupply(0, halving_height)) return false;
        }

        // Tail emission: at deep heights, subsidy should be exactly 1 DIN
        uint32_t deep_tail_height = 1 + (7 * ConsensusSubsidy::HALVING_INTERVAL) + 1000;
        AmountUna deep_subsidy = ConsensusSubsidy::GetBlockSubsidy(deep_tail_height);
        if (deep_subsidy.GetUna() != ConsensusSubsidy::TAIL_EMISSION_UNA) return false;

        return true;
    }

    /**
     * Get human-readable supply summary at height
     *
     * @param height Block height
     * @return String describing supply state
     */
    static std::string GetSupplySummary(uint32_t height) {
        uint64_t issued = ConsensusSubsidy::GetTotalIssuedAtHeight(height);
        AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

        char buffer[256];
        snprintf(buffer, sizeof(buffer),
                 "Height %u: %.2f DIN issued, current subsidy: %.8f DIN",
                 height,
                 issued / 1e8,
                 subsidy.GetUna() / 1e8);
        return std::string(buffer);
    }
};

} // namespace dinero
