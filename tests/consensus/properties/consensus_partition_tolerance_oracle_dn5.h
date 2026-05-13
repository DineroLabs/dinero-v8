#pragma once

#include "consensus_partition_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DN5Oracle - Cascading Partitions Property
 *
 * Property: Multiple sequential partitions eventually converge
 *           to a consistent final state
 *
 * Cascading Partitions Definition:
 * - Network experiences multiple partitions over time
 * - Partition 1 occurs and heals
 * - Partition 2 occurs and heals
 * - ... (repeat)
 * - After all partitions heal, all nodes converge
 *
 * Violation Detection:
 * - Detect multiple partition/heal cycles
 * - After final heal, check if all nodes converged
 * - If not → DN5 violation
 *
 * Why DN5 Matters:
 * - Real networks experience repeated partitions
 * - Ensures recovery from complex partition scenarios
 * - Tests resilience to partition storms
 * - Validates healing doesn't accumulate errors
 *
 * Example Scenario (No Violation):
 * - T=100: Partition {alice,bob} vs {carol}
 * - T=200: Heal
 * - T=300: Partition {alice,carol} vs {bob}
 * - T=400: Heal
 * - T=500: All nodes converged ✓
 *
 * Example Scenario (Violation):
 * - Multiple partition/heal cycles
 * - After final heal: alice at height 10, bob at height 12
 * - Nodes diverged ✗
 *
 * Phase 5d Scope:
 * - Simplified: Check if nodes converged after all partitions
 * - Full implementation: Track state evolution through each cycle
 */
class DN5Oracle : public PartitionToleranceOracle {
public:
    DN5Oracle() = default;

    std::string getName() const override {
        return "DN5: Cascading Partitions";
    }

protected:
    std::vector<PartitionViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    /**
     * Count number of partition/heal cycles in trace
     */
    size_t countPartitionCycles(const ConsensusTrace& trace) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
