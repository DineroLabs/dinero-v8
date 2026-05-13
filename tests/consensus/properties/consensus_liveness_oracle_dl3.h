#pragma once

#include "consensus_liveness_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DL3Oracle - Chain Growth Property
 *
 * Property: Chain height increases monotonically - no extended stalls
 *           in block production
 *
 * Growth Definition:
 * - At least one honest node must increase height within growth_timeout
 * - "Stall" = no height increase across all honest nodes
 * - Tolerates temporary mining pauses, but not indefinite stalls
 *
 * Violation Detection:
 * - Track max chain height over time across all honest nodes
 * - If max height unchanged for > growth_timeout → violation
 *
 * Why DL3 Matters:
 * - Ensures liveness of the blockchain
 * - Prevents deadlock scenarios
 * - Guarantees transaction finality eventually happens
 * - Detects mining failures or network-wide issues
 *
 * Example Scenario (Violation):
 * - T=0: All nodes at height 10
 * - T=1000: Still at height 10 (no mining)
 * - T=2000: Still at height 10 (stall continues)
 * - T=2001: Timeout exceeded (growth_timeout=2000ms)
 * - DL3 violation: Chain stalled for 2000ms
 *
 * Example Scenario (No Violation):
 * - T=0: All nodes at height 10
 * - T=1500: Alice mines block 11
 * - T=1800: No growth since T=1500 (only 300ms)
 * - No violation: Growth occurred within timeout
 *
 * Timeout Considerations:
 * - Must account for block time (e.g., Bitcoin ~10min)
 * - Dinero regtest: fast mining, short timeout acceptable
 * - Default: 2000ms (generous for regtest)
 */
class DL3Oracle : public ConsensusLivenessOracle {
public:
    /**
     * Create DL3 oracle
     *
     * @param growth_timeout Max time without height increase (ms)
     */
    explicit DL3Oracle(uint64_t growth_timeout = 2000)
        : growth_timeout_(growth_timeout)
    {}

    std::string getName() const override {
        return "DL3: Chain Growth";
    }

protected:
    std::vector<LivenessViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint64_t growth_timeout_;

    /**
     * Get maximum chain height at a specific timestamp
     */
    uint32_t getMaxHeightAtTime(
        const ConsensusTrace& trace,
        uint64_t timestamp,
        const std::vector<NodeID>& honest_nodes
    ) const;

    /**
     * Detect periods where chain height didn't grow
     */
    std::vector<std::pair<uint64_t, uint64_t>> findStallPeriods(
        const ConsensusTrace& trace,
        const std::vector<NodeID>& honest_nodes
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
