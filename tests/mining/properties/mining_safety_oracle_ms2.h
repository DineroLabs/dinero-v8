#pragma once

#include "mining_safety_oracle.h"
#include <map>
#include <vector>

// Ring 4 Phase 4d: MS2 - No Duplicate Subsidy
// Property: Each height should only claim subsidy once (even across reorgs)

namespace mining_test {

/**
 * MS2Oracle - No Duplicate Subsidy
 *
 * Safety property: ∀ heights H, count(subsidy_claimed_at_height[H]) <= 1
 *
 * Rationale:
 * - Each height represents one block position in the chain
 * - Only one block should claim subsidy at each height
 * - Reorgs should not allow duplicate subsidy claims
 * - Multiple solutions at same height indicates fork or bug
 *
 * Detection strategy:
 * - Track all solutions found at each height
 * - Track subsidy claimed at each height
 * - Flag if multiple different blocks accepted at same height
 *
 * Phase 4d scope:
 * - Full solution tracking by height
 * - Conservative detection (may flag legitimate reorgs)
 * - Phase 4h will add full fork tracking
 */
class MS2Oracle : public MiningSafetyOracle {
public:
    explicit MS2Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track solutions by height
    // Map: height -> list of (block_hash, event_index)
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> solutions_by_height_;

    // Track accepted blocks by height
    // Map: height -> list of (block_hash, event_index)
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>> accepted_by_height_;

    // Track subsidy claims by height
    // Map: height -> total subsidy claimed
    std::map<uint32_t, uint64_t> subsidy_claimed_by_height_;
};

}  // namespace mining_test
