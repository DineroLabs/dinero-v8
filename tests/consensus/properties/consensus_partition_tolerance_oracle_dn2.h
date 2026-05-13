#pragma once

#include "consensus_partition_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DN2Oracle - Minority Stall Property
 *
 * Property: Minority partitions produce only orphan blocks that
 *           get discarded when the partition heals
 *
 * Minority Stall Definition:
 * - Minority partition = group with ≤50% of honest nodes
 * - Minority can mine blocks during partition
 * - But those blocks become orphans after healing
 * - Final chain = majority partition's chain
 *
 * Violation Detection:
 * - Partition occurs, creating minority partition(s)
 * - Minority mines blocks during partition
 * - Partition heals
 * - Check if minority's blocks are on final chain
 * - If minority blocks persist → DN2 violation
 *
 * Why DN2 Matters:
 * - Ensures longest chain rule works correctly
 * - Prevents minority from hijacking consensus
 * - Guarantees finality when majority agrees
 * - Security property: minority can't override majority
 *
 * Example Scenario (No Violation):
 * - Network: alice, bob, carol (3 nodes)
 * - Partition at T=100: {alice, bob} vs {carol}
 * - Minority: {carol} (1/3 = 33% < 50%)
 * - Carol mines block_c at height 10
 * - Partition heals at T=500
 * - Final chain: alice/bob's chain (block_c orphaned) ✓
 *
 * Example Scenario (Violation):
 * - Network: alice, bob, carol (3 nodes)
 * - Partition at T=100: {alice, bob} vs {carol}
 * - Minority: {carol}
 * - Carol mines block_c
 * - Partition heals
 * - Final chain: includes block_c (minority block survived!) ✗
 *
 * Phase 5d Scope:
 * - Simplified: Check if all nodes converged to majority's chain
 * - Full implementation: Track orphan blocks explicitly
 */
class DN2Oracle : public PartitionToleranceOracle {
public:
    DN2Oracle() = default;

    std::string getName() const override {
        return "DN2: Minority Stall";
    }

protected:
    std::vector<PartitionViolation> observeTrace(const ConsensusTrace& trace) override;
};

} // namespace test
} // namespace consensus
} // namespace dinero
