#pragma once

#include "mining_determinism_oracle.h"

// Ring 4 Phase 4f: MD4 - No Hidden Entropy Sources
// Property: All nondeterminism derives exclusively from the seed

namespace mining_test {

/**
 * MD4Oracle - No Hidden Entropy Sources
 *
 * Determinism property: ∀ seed S, environmental perturbations E,
 *                       Running(S) with E produces identical trace
 *
 * Rationale:
 * - All randomness must originate from explicit seed
 * - No hidden entropy sources allowed (time, addresses, OS, etc.)
 * - Same seed must produce same trace regardless of environment
 * - Different behavior must imply different seed
 *
 * Detection strategy:
 * - Run same seed multiple times
 * - Verify identical traces despite environmental differences
 * - Any divergence with same seed = hidden entropy
 *
 * What MD4 catches:
 * - time(nullptr) or std::chrono usage
 * - rand() / random_device usage
 * - Address-based ordering (pointer comparisons)
 * - Unordered container iteration
 * - Hash salt usage
 * - OS entropy calls
 * - Accidental reseeding
 * - Static initialization order bugs
 *
 * What MD4 requires:
 * - Identical seeds
 * - All randomness from seed
 * - No external entropy
 *
 * MD4 vs MD1:
 * - MD1: "Same seed → same trace" (determinism in practice)
 * - MD4: "Different behavior → different seed" (determinism in principle)
 *
 * MD4 is STRICTER than MD1.
 * This is the last line of defense before production.
 */
class MD4Oracle : public MiningDeterminismOracle {
public:
    MD4Oracle() = default;

    std::string name() const override;

    std::vector<DeterminismViolation> check(
        const MiningTrace& reference,
        const MiningTrace& candidate
    ) const override;
};

}  // namespace mining_test
