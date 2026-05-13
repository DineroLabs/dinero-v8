#pragma once

#include "consensus_liveness_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DL1Oracle - Eventual Consensus Property
 *
 * Property: After a network partition heals, all honest nodes eventually
 *           converge to the same chain state
 *
 * Convergence Definition:
 * - All honest nodes have the same chain tip hash
 * - All honest nodes have the same chain height
 * - Within a reasonable timeout after partition healing
 *
 * Violation Detection:
 * - Partition heals at time T_heal
 * - By time T_heal + timeout, check if all honest nodes converged
 * - If not, report liveness violation
 *
 * Why DL1 Matters:
 * - Fundamental liveness guarantee
 * - Ensures network can recover from partitions
 * - Prevents permanent splits
 * - Foundation for consistency guarantees
 *
 * Example Scenario:
 * - Network partitions at T=100: {alice, bob} vs {carol}
 * - Alice and Bob mine on chain A
 * - Carol mines on chain C
 * - Partition heals at T=500
 * - By T=600 (timeout=100), all nodes should agree on one chain
 * - If they don't → DL1 violation
 *
 * Timeout Considerations:
 * - Too short: False positives (nodes need time to sync)
 * - Too long: Weak guarantee (defeats purpose of liveness)
 * - Default: 1000ms (configurable based on network latency)
 */
class DL1Oracle : public ConsensusLivenessOracle {
public:
    /**
     * Create DL1 oracle
     *
     * @param convergence_timeout Time after partition heal to achieve consensus (ms)
     */
    explicit DL1Oracle(uint64_t convergence_timeout = 1000)
        : convergence_timeout_(convergence_timeout)
    {}

    std::string getName() const override {
        return "DL1: Eventual Consensus";
    }

protected:
    std::vector<LivenessViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint64_t convergence_timeout_;

    /**
     * Check if nodes converged by a specific deadline
     */
    bool didConvergeByDeadline(
        const ConsensusTrace& trace,
        uint64_t deadline
    ) const;

    /**
     * Get the time when all nodes converged (if they did)
     */
    std::optional<uint64_t> getConvergenceTime(const ConsensusTrace& trace) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
