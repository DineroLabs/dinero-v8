#pragma once

#include "consensus_byzantine_tolerance_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DB1Oracle - Network Resilience
 *
 * Property: Network makes progress despite presence of Byzantine nodes
 *
 * Observable Network Resilience Definition:
 * - Byzantine nodes are present (is_byzantine=true)
 * - Network still produces blocks
 * - No assumptions about Byzantine strategy or intent
 *
 * Violation Detection:
 * - Byzantine nodes exist in trace
 * - Check if network produced any blocks
 * - If no blocks → DB1 violation (network stalled due to Byzantine presence)
 *
 * Why DB1 Matters:
 * - Fundamental Byzantine fault tolerance guarantee
 * - Ensures network doesn't completely fail when malicious nodes exist
 * - Observable resilience check: Did progress happen or not?
 * - No policy assumptions about Byzantine behavior
 *
 * Example Scenario (No Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve present throughout execution
 * - Alice mines block at T=100 ✓
 * - Network showed resilience
 *
 * Example Scenario (Violation):
 * - Network: alice (honest), bob (honest), eve (Byzantine)
 * - Eve becomes Byzantine at T=50
 * - No blocks mined by anyone after T=50
 * - Network stalled due to Byzantine presence ✗
 *
 * Phase 5e Scope:
 * - Observable only: Did blocks get produced while Byzantine nodes existed?
 * - No inference about why Byzantine nodes might have caused stall
 * - Check outcome, not intent
 */
class DB1Oracle : public ByzantineToleranceOracle {
public:
    /**
     * Create DB1 oracle
     */
    DB1Oracle() = default;

    std::string getName() const override {
        return "DB1: Network Resilience";
    }

protected:
    std::vector<ByzantineViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    /**
     * Get timestamp when first Byzantine node appeared
     */
    std::optional<uint64_t> getByzantineStartTime(const ConsensusTrace& trace) const;

    /**
     * Check if network produced blocks after Byzantine nodes appeared
     */
    bool didNetworkProduceBlocksAfterByzantine(
        const ConsensusTrace& trace,
        uint64_t byzantine_start_time
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
