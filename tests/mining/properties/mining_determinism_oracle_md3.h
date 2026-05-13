#pragma once

#include "mining_determinism_oracle.h"

// Ring 4 Phase 4f: MD3 - Action Commutativity (Where Allowed)
// Property: Independent actions can be reordered without changing outcome

namespace mining_test {

/**
 * MD3Oracle - Action Commutativity (Where Allowed)
 *
 * Determinism property: ∀ independent action pairs (A, B),
 *                       Sequence [A, B] produces same trace as [B, A]
 *
 * Rationale:
 * - If actions are declared independent, order shouldn't matter
 * - Hidden coupling between "independent" actions indicates bugs
 * - Accidental ordering dependencies are catastrophic in production
 * - Non-deterministic container iteration causes subtle failures
 *
 * Detection strategy:
 * - Compare two traces: original order vs swapped order
 * - If traces diverge, independence claim is violated
 * - Oracle does NOT perform swap (test harness responsibility)
 * - Uses same comparison logic as MD1
 *
 * What MD3 catches:
 * - Unordered container iteration (std::unordered_map, etc.)
 * - Hidden state coupling between "independent" actions
 * - Global counter updates that create order dependencies
 * - Implicit coupling through shared caches
 * - Latent race-like bugs (even in single-threaded code)
 *
 * What MD3 requires:
 * - Explicit independence declarations
 * - Test harness generates action pairs
 * - Same seed for both runs
 *
 * This is the SUBTLEST determinism property.
 * MD3 catches bugs that MD1/MD2 cannot see.
 */
class MD3Oracle : public MiningDeterminismOracle {
public:
    MD3Oracle() = default;

    std::string name() const override;

    std::vector<DeterminismViolation> check(
        const MiningTrace& reference,
        const MiningTrace& candidate
    ) const override;
};

}  // namespace mining_test
