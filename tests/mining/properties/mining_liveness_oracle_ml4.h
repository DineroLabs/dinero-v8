#pragma once

#include "mining_liveness_oracle.h"

// Ring 4 Phase 4e: ML4 - Mining Eventually Restarts
// Property: After crash, mining eventually resumes

namespace mining_test {

/**
 * ML4Oracle - Mining Eventually Restarts
 *
 * Liveness property: ∀ CRASH events followed by RESTART,
 *                    ∃ POW_STARTED event within reasonable window
 *
 * Rationale:
 * - When system crashes and restarts, mining should eventually resume
 * - Stuck state after restart indicates:
 *   - Initialization failure
 *   - Configuration corruption
 *   - State recovery failure
 * - Forward progress requires mining to resume after recovery
 *
 * Detection strategy:
 * - Track CRASH events (system goes down)
 * - Track RESTART events (system comes back up)
 * - Track POW_STARTED events (mining resumes)
 * - Flag if system restarted but mining never resumes
 * - Use event count as time proxy
 *
 * Phase 4e threshold:
 * - 100 events after restart without POW_STARTED → violation
 * - Conservative bound (allows slow initialization)
 * - In normal operation, mining should resume quickly after restart
 * - Phase 4g will add real timing and startup delays
 *
 * Important distinction:
 * - ML1: Templates eventually created (mining → template)
 * - ML2: Solutions eventually found (template → solution)
 * - ML3: Blocks eventually submitted (solution → submission)
 * - ML4: Mining eventually restarts (crash → mining)
 * - ML5: Stale templates eventually discarded (reorg → cleanup)
 */
class ML4Oracle : public MiningLivenessOracle {
public:
    explicit ML4Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track restart state
    bool system_restarted_{false};
    bool mining_resumed_{false};
    uint64_t events_since_restart_{0};
    uint64_t restart_event_index_{0};
    uint64_t total_crashes_{0};
    uint64_t total_restarts_{0};
    uint64_t last_restart_count_{0};

    // Threshold for liveness violation (event count)
    static constexpr uint64_t kRestartThreshold = 100;
};

}  // namespace mining_test
