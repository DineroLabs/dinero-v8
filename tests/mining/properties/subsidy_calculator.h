#pragma once

#include "consensus_params.h"
#include <cstdint>

// Ring 4 Phase 4c: Consensus Subsidy Calculator
// Purpose: Calculate expected subsidy for property validation
// Source: Copied from Ring 1 frozen consensus specification
// Rule: Read-only, never modify consensus logic

namespace mining_test {

// ============================================================================
// ConsensusSubsidyCalculator - Expected subsidy calculation
// ============================================================================

class ConsensusSubsidyCalculator {
public:
    // Construct with consensus parameters
    explicit ConsensusSubsidyCalculator(const ConsensusParams& params);

    // Calculate block subsidy for given height
    // Returns subsidy in una (1 DIN = 100,000,000 una)
    uint64_t getBlockSubsidy(uint32_t height) const;

    // Get halving interval
    uint32_t getHalvingInterval() const { return params_.halving_interval; }

    // Get initial subsidy
    uint64_t getInitialSubsidy() const { return params_.initial_subsidy; }

    // Get genesis subsidy
    uint64_t getGenesisSubsidy() const { return params_.genesis_subsidy; }

    // Calculate number of halvings that have occurred at given height
    uint32_t getHalvingsAt(uint32_t height) const;

    // Check if height is at halving boundary
    bool isHalvingHeight(uint32_t height) const;

    // Get consensus params
    const ConsensusParams& getParams() const { return params_; }

private:
    ConsensusParams params_;
};

}  // namespace mining_test
