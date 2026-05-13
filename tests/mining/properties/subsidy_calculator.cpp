#include "subsidy_calculator.h"

// Ring 4 Phase 4c: Subsidy Calculator Implementation
// Rule: Matches Ring 1 consensus logic exactly

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

ConsensusSubsidyCalculator::ConsensusSubsidyCalculator(const ConsensusParams& params)
    : params_(params) {
}

// ============================================================================
// Subsidy Calculation (Ring 1 Frozen Logic)
// ============================================================================

uint64_t ConsensusSubsidyCalculator::getBlockSubsidy(uint32_t height) const {
    // Genesis block special case
    if (height == 0) {
        return params_.genesis_subsidy;
    }

    // Calculate number of halvings that have occurred
    uint32_t halvings = height / params_.halving_interval;

    // Subsidy becomes zero after 64 halvings (or when it rounds to 0)
    // This is because right-shifting by 64 or more is undefined behavior
    if (halvings >= 64) {
        return 0;
    }

    // Start with initial subsidy
    uint64_t subsidy = params_.initial_subsidy;

    // Halve subsidy for each halving period
    // Right shift by N is equivalent to dividing by 2^N
    subsidy >>= halvings;

    return subsidy;
}

// ============================================================================
// Helper Methods
// ============================================================================

uint32_t ConsensusSubsidyCalculator::getHalvingsAt(uint32_t height) const {
    // Genesis block has no halvings
    if (height == 0) {
        return 0;
    }

    return height / params_.halving_interval;
}

bool ConsensusSubsidyCalculator::isHalvingHeight(uint32_t height) const {
    // Genesis is not a halving height
    if (height == 0) {
        return false;
    }

    // Height is at halving boundary if it's divisible by halving_interval
    return (height % params_.halving_interval) == 0;
}

}  // namespace mining_test
