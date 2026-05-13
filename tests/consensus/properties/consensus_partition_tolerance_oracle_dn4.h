#pragma once

#include "consensus_partition_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DN4Oracle - Asynchronous Healing Property
 *
 * Property: The final consensus state is independent of the order
 *           in which partition boundaries heal
 *
 * Asynchronous Healing Definition:
 * - Partitions can heal in any order (non-deterministic timing)
 * - Final state should be deterministic (longest chain wins)
 * - Order of healing shouldn't affect final chain
 *
 * Violation Detection:
 * - Run same scenario with different healing orders
 * - Compare final chain states
 * - If different → DN4 violation
 *
 * Why DN4 Matters:
 * - Ensures consistency despite asynchronous networks
 * - Prevents timing-dependent outcomes
 * - Foundation for determinism guarantees
 * - Critical for reproducibility
 *
 * Example Scenario (No Violation):
 * - 3-way partition: {alice} vs {bob} vs {carol}
 * - Healing order 1: alice↔bob, then all↔carol
 * - Healing order 2: bob↔carol, then all↔alice
 * - Final chain: same in both cases ✓
 *
 * Example Scenario (Violation):
 * - 3-way partition
 * - Healing order 1 → final chain: block_a
 * - Healing order 2 → final chain: block_b
 * - Different outcomes ✗
 *
 * Phase 5d Scope:
 * - Simplified: Check if all nodes converged (deterministic outcome)
 * - Full implementation: Replay with different heal orders
 */
class DN4Oracle : public PartitionToleranceOracle {
public:
    DN4Oracle() = default;

    std::string getName() const override {
        return "DN4: Asynchronous Healing";
    }

protected:
    std::vector<PartitionViolation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
