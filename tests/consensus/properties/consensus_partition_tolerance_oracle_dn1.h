#pragma once

#include "consensus_partition_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DN1Oracle - Network Liveness During Partition
 *
 * Property: During a network partition, at least one partition
 *           produces blocks (network doesn't completely stall)
 *
 * Observable Network Liveness Definition:
 * - Network splits into groups (partitions)
 * - "Liveness" = at least one block produced network-wide
 * - No assumptions about which partition should progress
 *
 * Violation Detection:
 * - Partition occurs at time T_partition
 * - Check if ANY node produced blocks during partition
 * - If no blocks network-wide → DN1 violation (total stall)
 *
 * Why DN1 Matters:
 * - Fundamental availability guarantee
 * - Ensures network doesn't completely deadlock during partitions
 * - Observable network-wide liveness check
 * - No policy assumptions about which partition should progress
 *
 * Example Scenario (No Violation):
 * - Network: alice, bob, carol (3 nodes)
 * - Partition at T=100: {alice, bob} vs {carol}
 * - Alice mines block at T=200 ✓
 * - OR Carol mines block at T=200 ✓
 * - Network showed liveness
 *
 * Example Scenario (Violation):
 * - Network: alice, bob, carol (3 nodes)
 * - Partition at T=100: {alice, bob} vs {carol}
 * - No blocks mined by anyone by T=500 (partition heals)
 * - Total network stall ✗
 *
 * Phase 5d Scope:
 * - Observable only: Did any blocks get produced?
 * - No majority/minority distinction required
 */
class DN1Oracle : public PartitionToleranceOracle {
public:
    /**
     * Create DN1 oracle
     */
    DN1Oracle() = default;

    std::string getName() const override {
        return "DN1: Network Liveness During Partition";
    }

protected:
    std::vector<PartitionViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    /**
     * Check if ANY blocks were produced during partition period
     */
    bool didNetworkProduceBlocks(
        const ConsensusTrace& trace,
        uint64_t start_time,
        uint64_t end_time
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
