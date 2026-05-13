#pragma once

#include "mining_safety_oracle.h"
#include <map>
#include <vector>

// Ring 4 Phase 4d: MS5 - No Stale Block Acceptance
// Property: Old templates must be discarded after reorgs

namespace mining_test {

/**
 * MS5Oracle - No Stale Block Acceptance
 *
 * Safety property: ∀ reorg events, old templates are discarded
 *
 * Rationale:
 * - After reorg, chain tip changes
 * - Mining should build on new tip, not old tip
 * - Accepting stale blocks indicates mining on outdated chain
 * - Stale work should be discarded and templates recreated
 *
 * Detection strategy (Phase 4d - PLACEHOLDER):
 * - Track current chain tip
 * - Track reorg events (tip changes)
 * - Track template creation times and tips
 * - Flag if block accepted building on old tip
 * - Full fork tracking deferred to Phase 4h
 *
 * Phase 4d scope:
 * - Basic tip tracking only
 * - Conservative detection (tip consistency)
 * - Placeholder validation (no full fork tree)
 * - Phase 4h will add full fork tracking
 *
 * What MS5 prevents NOW:
 * - Mining on stale tips after reorg
 * - Accepting blocks from old templates
 * - Wasted work on abandoned forks
 * - Chain state inconsistency
 *
 * What MS5 will enforce in Phase 4h:
 * - Full fork tree tracking
 * - Block parent validation
 * - Chain reorganization handling
 * - Orphan block detection
 */
class MS5Oracle : public MiningSafetyOracle {
public:
    explicit MS5Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track current chain tip
    uint64_t current_tip_{0};

    // Track reorg count
    uint32_t reorg_count_{0};

    // Track templates created and their tips
    // Map: height -> (template_tip, event_index)
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> templates_by_height_;

    // Track blocks accepted and their tips
    // Map: height -> (block_tip, event_index)
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> accepted_by_height_;
};

}  // namespace mining_test
