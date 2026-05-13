#pragma once

#include "mining_liveness_oracle.h"

// Ring 4 Phase 4e: ML3 - Blocks Eventually Submitted
// Property: Found solutions eventually get submitted to the network

namespace mining_test {

/**
 * ML3Oracle - Blocks Eventually Submitted
 *
 * Liveness property: ∀ SOLUTION_FOUND events,
 *                    ∃ BLOCK_SUBMITTED event within reasonable window
 *
 * Rationale:
 * - When a valid solution is found, it should be submitted to the network
 * - Stuck solutions (not submitted) indicate:
 *   - Network submission pipeline failure
 *   - Block serialization failure
 *   - Validation pipeline deadlock
 * - Forward progress requires submission to network
 *
 * Detection strategy:
 * - Track SOLUTION_FOUND events
 * - Track BLOCK_SUBMITTED events
 * - Flag if solution found but not submitted within window
 * - Use event count as time proxy
 *
 * Phase 4e threshold:
 * - 50 events without submission → violation
 * - Quick threshold because submission should be immediate
 * - In normal operation, BLOCK_SUBMITTED follows SOLUTION_FOUND immediately
 * - Phase 4g will add real timing and network delays
 *
 * Important distinction:
 * - ML2: Solutions eventually found (mining → solution)
 * - ML3: Blocks eventually submitted (solution → submission)
 * - ML4: Mining eventually restarts (crash → restart)
 */
class ML3Oracle : public MiningLivenessOracle {
public:
    explicit ML3Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track solution submission state
    bool solution_pending_{false};
    uint64_t events_since_solution_{0};
    uint64_t solution_event_index_{0};
    uint64_t total_solutions_{0};
    uint64_t total_submissions_{0};

    // Threshold for liveness violation (event count)
    // Lower than ML2 because submission should be immediate
    static constexpr uint64_t kSubmissionThreshold = 50;
};

}  // namespace mining_test
