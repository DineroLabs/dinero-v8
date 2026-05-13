#pragma once

#include "mining_liveness_oracle.h"

// Ring 4 Phase 4e: ML5 - Stale Templates Eventually Discarded
// Property: When chain tip changes, old templates eventually discarded

namespace mining_test {

/**
 * ML5Oracle - Stale Templates Eventually Discarded
 *
 * Liveness property: ∀ chain tip changes (NEW_BLOCK_ARRIVED),
 *                    ∃ TEMPLATE_DISCARDED event within reasonable window
 *
 * Rationale:
 * - When new blocks arrive, old templates become stale
 * - Stale templates waste hash power (mining on wrong chain)
 * - Templates should be discarded quickly after tip change
 * - Forward progress requires mining on current chain tip
 *
 * Detection strategy:
 * - Track chain tip changes via state.current_tip
 * - Track template validity via state.template_height
 * - Track TEMPLATE_DISCARDED events
 * - Flag if tip changes but stale template not discarded
 * - Use event count as time proxy
 *
 * Phase 4e threshold:
 * - 50 events without discard → violation
 * - Quick threshold because template discard should be immediate
 * - In normal operation, templates discarded when new block arrives
 * - Phase 4g will add real timing and network delays
 *
 * Important distinction:
 * - ML1: Templates eventually created (mining start → template)
 * - ML5: Stale templates eventually discarded (tip change → discard)
 * - Both are about template lifecycle, but opposite directions
 *
 * Note: This is a placeholder for Phase 4h
 * - Phase 4h will track full fork tree
 * - For now, we track simple tip changes
 */
class ML5Oracle : public MiningLivenessOracle {
public:
    explicit ML5Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track template discard state
    bool tip_changed_{false};
    bool template_active_{false};
    bool template_discarded_{false};
    uint64_t events_since_tip_change_{0};
    uint64_t tip_change_event_index_{0};
    uint64_t last_tip_{0};
    uint64_t total_tip_changes_{0};
    uint64_t total_template_discards_{0};

    // Threshold for liveness violation (event count)
    // Lower than ML1 because discard should be immediate
    static constexpr uint64_t kDiscardThreshold = 50;
};

}  // namespace mining_test
