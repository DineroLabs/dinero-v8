#pragma once

#include "mining_determinism_oracle.h"

// Ring 4 Phase 4f: MD5 - Deterministic Crash Recovery
// Property: Crash recovery path is deterministic

namespace mining_test {

/**
 * MD5Oracle - Deterministic Crash Recovery
 *
 * Determinism property: ∀ seed S, crash index C,
 *                       Recovery path after crash at C is deterministic
 *
 * Rationale:
 * - Crash recovery must be reproducible
 * - Same crash point must lead to same recovery behavior
 * - Recovery path must not introduce hidden entropy
 * - Crash injection at same index must produce identical traces
 *
 * Detection strategy:
 * - Inject crash at specific index in both traces
 * - Compare recovery path after crash
 * - Verify identical post-crash behavior
 * - Any divergence = non-deterministic recovery
 *
 * What MD5 catches:
 * - Non-deterministic recovery logic
 * - Hidden state in crash handlers
 * - Uncontrolled recovery ordering
 * - Recovery path entropy sources
 * - State corruption during recovery
 * - Non-reproducible crash handling
 *
 * What MD5 requires:
 * - Identical crash injection points
 * - Deterministic recovery path
 * - No hidden recovery state
 *
 * MD5 vs MD2:
 * - MD2: "Restart replay produces same suffix" (general restart)
 * - MD5: "Crash recovery is deterministic" (explicit crash injection)
 *
 * MD5 is the CAPSTONE of Phase 4f.
 * This is the final determinism property before production.
 */
class MD5Oracle : public MiningDeterminismOracle {
public:
    MD5Oracle() = default;

    std::string name() const override;

    std::vector<DeterminismViolation> check(
        const MiningTrace& reference,
        const MiningTrace& candidate
    ) const override;

private:
    /**
     * Find crash recovery boundary in trace
     * Returns index of first event after crash recovery
     */
    size_t findCrashRecoveryBoundary(const MiningTrace& trace) const;
};

}  // namespace mining_test
