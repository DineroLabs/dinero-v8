#pragma once

#include "consensus_partition_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DN3Oracle - Clean Healing Property
 *
 * Property: When a partition heals, no blocks are lost -
 *           all blocks eventually appear on some node's chain view
 *
 * Clean Healing Definition:
 * - During partition, different groups mine different blocks
 * - When partition heals, all blocks are visible
 * - Longest chain rule determines main chain
 * - Losing chains become orphans, but blocks aren't deleted
 *
 * Violation Detection:
 * - Track all blocks mined during partition
 * - After partition heals, verify all blocks exist somewhere
 * - If any block disappeared → DN3 violation
 *
 * Why DN3 Matters:
 * - Data integrity guarantee
 * - Ensures no information loss during healing
 * - Foundation for partition recovery
 * - Prevents silent data corruption
 *
 * Example Scenario (No Violation):
 * - Partition: {alice, bob} mines block_a, {carol} mines block_c
 * - Partition heals
 * - Final: All nodes have block_a on main chain
 * - All nodes know block_c exists (as orphan)
 * - No blocks lost ✓
 *
 * Example Scenario (Violation):
 * - Partition: {alice, bob} mines block_a, {carol} mines block_c
 * - Partition heals
 * - block_c completely disappears (not even as orphan)
 * - Data lost ✗
 *
 * Phase 5d Scope:
 * - Simplified: Check if all nodes reached same height
 * - Full implementation: Track block inventory across nodes
 */
class DN3Oracle : public PartitionToleranceOracle {
public:
    DN3Oracle() = default;

    std::string getName() const override {
        return "DN3: Clean Healing";
    }

protected:
    std::vector<PartitionViolation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
