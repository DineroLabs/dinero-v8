#pragma once

#include "mining_liveness_oracle.h"

// Ring 4 Phase 4e: ML1 - Templates Eventually Created
// Property: Mining activity eventually produces templates

namespace mining_test {

/**
 * ML1Oracle - Templates Eventually Created
 *
 * Liveness property: ∀ START_MINING actions,
 *                    ∃ TEMPLATE_CREATED event within reasonable window
 *
 * Rationale:
 * - When mining starts, templates should eventually be created
 * - Stuck mining (no templates) indicates deadlock or failure
 * - Forward progress requires template creation
 *
 * Detection strategy:
 * - Track START_MINING actions
 * - Track TEMPLATE_CREATED events
 * - Flag if mining runs without creating templates
 * - Use event count as time proxy
 *
 * Phase 4e threshold:
 * - 100 events without template creation → violation
 * - Conservative bound (allows slow progress)
 * - Phase 4g will add real timing
 */
class ML1Oracle : public MiningLivenessOracle {
public:
    explicit ML1Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track mining state
    bool mining_started_{false};
    uint64_t events_since_mining_start_{0};
    uint64_t mining_start_event_index_{0};
    bool template_created_{false};

    // Threshold for liveness violation (event count)
    static constexpr uint64_t kTemplateCreationThreshold = 100;
};

}  // namespace mining_test
