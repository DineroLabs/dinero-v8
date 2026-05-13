#pragma once

#include "consensus_byzantine_tolerance_oracle.h"
#include <unordered_set>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DB5Oracle - Invalid Block Rejection
 *
 * Property: Honest nodes reject invalid blocks from Byzantine nodes
 *
 * Observable Invalid Block Rejection Definition:
 * - Byzantine nodes create invalid blocks (violate consensus rules)
 * - Invalid = wrong PoW, invalid txs, broken chain rules
 * - Honest nodes validate and reject invalid blocks
 * - No assumptions about Byzantine invalidity strategy
 *
 * Violation Detection:
 * - Identify invalid blocks (INVALID_BLOCK_CREATED or validation failures)
 * - Check if any honest nodes accepted these invalid blocks
 * - If honest node accepted invalid block → DB5 violation
 *
 * Why DB5 Matters:
 * - Fundamental validation integrity
 * - Prevents Byzantine nodes from corrupting honest node state
 * - Observable safety check: Did honest nodes accept invalid blocks?
 * - No inference about Byzantine block creation strategy
 *
 * Example Scenario (No Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve creates invalid block (wrong PoW) at T=100
 * - Alice validates and rejects block ✓
 * - Bob validates and rejects block ✓
 * - Invalid blocks properly rejected
 *
 * Example Scenario (Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve creates invalid block (invalid transaction) at T=100
 * - Alice accepts block without proper validation ✗
 * - Invalid block accepted by honest node
 *
 * Phase 5e Scope:
 * - Observable only: Did honest nodes accept invalid blocks?
 * - Check block validation events
 * - Check acceptance by honest nodes
 * - No inference about why validation failed
 */
class DB5Oracle : public ByzantineToleranceOracle {
public:
    /**
     * Create DB5 oracle
     */
    DB5Oracle() = default;

    std::string getName() const override {
        return "DB5: Invalid Block Rejection";
    }

protected:
    std::vector<ByzantineViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    /**
     * Find all invalid blocks (marked as invalid in trace)
     */
    std::unordered_set<std::string> getInvalidBlocks(
        const ConsensusTrace& trace
    ) const;

    /**
     * Check if a specific block was accepted by an honest node
     */
    bool wasBlockAcceptedByHonestNode(
        const ConsensusTrace& trace,
        const std::string& block_hash
    ) const;

    /**
     * Get nodes that accepted a specific block
     */
    std::vector<NodeID> getNodesThatAcceptedBlock(
        const ConsensusTrace& trace,
        const std::string& block_hash
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
