#pragma once

#include "mining_safety_oracle.h"
#include <set>

// Ring 4 Phase 4d: MS4 - Consensus Always Enforced (PLACEHOLDER)
// Property: Block acceptance must be preceded by validation steps

namespace mining_test {

/**
 * MS4Oracle - Consensus Always Enforced (PLACEHOLDER)
 *
 * Safety property: ∀ blocks accepted, validation occurred first
 *
 * Rationale:
 * - Every accepted block must have passed validation
 * - No "fast paths" that skip validation
 * - Crash/restart must not bypass validation
 * - Procedural correctness (not semantic correctness)
 *
 * Detection strategy (Phase 4d - PLACEHOLDER):
 * - Track system crash state
 * - Ensure blocks not accepted while crashed
 * - Ensure blocks not accepted without template creation
 * - Flag validation bypass scenarios
 * - Full consensus validation deferred to Phase 4h
 *
 * Phase 4d scope:
 * - Procedural enforcement only
 * - Detects validation bypass bugs
 * - Catches restart fast-path errors
 * - Phase 4h will add full consensus enforcement
 *
 * What MS4 prevents NOW:
 * - Accepting blocks while system is crashed
 * - Accepting blocks without template creation
 * - Restart bypass bugs
 * - Fast-path shortcuts around validation
 *
 * What MS4 will enforce in Phase 4h:
 * - Full header validation
 * - Block structure validation
 * - Transaction validity
 * - UTXO + script checks
 */
class MS4Oracle : public MiningSafetyOracle {
public:
    explicit MS4Oracle(const ConsensusParams& params);

    std::string name() const override;
    void reset() override;
    void observe(const MiningState& state, const MiningEvent& event, uint64_t event_index) override;
    void finalize() override;

private:
    // Track crash state
    bool is_crashed_{false};

    // Track heights that had templates created (validation setup)
    std::set<uint32_t> heights_with_templates_;

    // Track heights that had solutions found (ready for validation)
    std::set<uint32_t> heights_with_solutions_;
};

}  // namespace mining_test
