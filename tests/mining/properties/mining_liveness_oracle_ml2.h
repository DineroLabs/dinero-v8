#pragma once

#include "mining_liveness_oracle.h"

// Ring 4 Phase 4e: ML2 - Solutions Eventually Found
// Property: Mining activity eventually produces solutions

namespace mining_test {

/**
 * ML2Oracle - Solutions Eventually Found
 *
 * Liveness property: ∀ active mining periods,
 *                    ∃ SOLUTION_FOUND event within reasonable window
 *
 * Rationale:
 * - When mining is active (hashing), solutions should eventually be found
 * - Stuck mining without solutions indicates:
 *   - Deadlock in solution checking
 *   - Hash generation failure
 *   - Difficulty too high / configuration error
 * - Forward progress requires solution discovery
 *
 * Detection strategy:
 * - Track when mining is active (POW_STARTED → POW_STOPPED)
 * - Track SOLUTION_FOUND events
 * - Flag if mining runs for extended period without solutions
 * - Use event count as time proxy
 *
 * Phase 4e threshold:
 * - 1000 events without solution → violation
 * - More lenient than ML1 (100) because solutions are probabilistic
 * - In regtest mode, difficulty is low, so solutions should be frequent
 * - Phase 4g will add real timing and difficulty adjustment
 *
 * Important distinction:
 * - ML1: Templates eventually created (deterministic, should be quick)
 * - ML2: Solutions eventually found (probabilistic, but still expected in regtest)
 * - ML3: Blocks eventually submitted (network-dependent)
 */
class ML2Oracle : public MiningLivenessOracle {
public:
    explicit ML2Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track mining state
    bool mining_active_{false};
    uint64_t events_since_mining_start_{0};
    uint64_t mining_start_event_index_{0};
    bool solution_found_{false};
    uint64_t total_solutions_{0};

    // Threshold for liveness violation (event count)
    // Higher than ML1 because solution finding is probabilistic
    static constexpr uint64_t kSolutionFindingThreshold = 1000;
};

}  // namespace mining_test
