#pragma once

#include "mining_safety_oracle.h"
#include <map>

// Ring 4 Phase 4d: MS1 - No Inflation Under Restart
// Property: Restarts must not create new subsidy from thin air

namespace mining_test {

/**
 * MS1Oracle - No Inflation Under Restart
 *
 * Safety property: ∀ restart events, total_subsidy_claimed <= total_subsidy_expected
 *
 * Rationale:
 * - Crash/restart should not duplicate subsidy
 * - After restart, next template should claim correct subsidy for its height
 * - Total subsidy claimed across all templates should match consensus rules
 *
 * Detection strategy:
 * - Track subsidy claimed in each TEMPLATE_CREATED event
 * - Track expected subsidy for each SOLUTION_FOUND event
 * - Flag if claimed > expected (inflation detected)
 *
 * Phase 4d scope:
 * - Full subsidy tracking with placeholder values
 * - Conservative detection (may flag false positives)
 * - Phase 4h will add real UTXO validation
 */
class MS1Oracle : public MiningSafetyOracle {
public:
    explicit MS1Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track subsidy claims
    uint64_t total_subsidy_claimed_{0};
    uint64_t total_subsidy_expected_{0};

    // Track restart events
    uint64_t restart_count_{0};
    bool currently_crashed_{false};

    // Track subsidy by height (for duplicate detection)
    std::map<uint32_t, uint64_t> subsidy_by_height_;
};

}  // namespace mining_test
