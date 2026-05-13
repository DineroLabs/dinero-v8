#pragma once

#include "consensus_liveness_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DL2Oracle - Block Propagation Property
 *
 * Property: After a block is broadcast, all honest nodes receive it
 *           within a bounded timeout
 *
 * Propagation Definition:
 * - Block broadcast at time T_broadcast
 * - All honest nodes receive by T_broadcast + timeout
 * - Network latency, packet loss tolerated within timeout
 *
 * Violation Detection:
 * - Block broadcast at T_broadcast
 * - Find nodes that didn't receive by T_broadcast + timeout
 * - Report propagation failure
 *
 * Why DL2 Matters:
 * - Essential for consensus convergence
 * - Prevents information asymmetry
 * - Detects network partition symptoms
 * - Foundation for fairness (all miners see same blocks)
 *
 * Example Scenario:
 * - Alice mines block_1 at T=100
 * - Bob receives at T=120 (latency=20ms) ✓
 * - Carol never receives (network partition) ✗
 * - By T=600 (timeout=500ms), Carol still hasn't received
 * - DL2 violation: block_1 failed to propagate to Carol
 *
 * Timeout Considerations:
 * - Too short: False positives on high-latency networks
 * - Too long: Weak guarantee (defeats purpose)
 * - Default: 500ms (reasonable for most networks)
 */
class DL2Oracle : public ConsensusLivenessOracle {
public:
    /**
     * Create DL2 oracle
     *
     * @param propagation_timeout Time for block to reach all nodes (ms)
     */
    explicit DL2Oracle(uint64_t propagation_timeout = 500)
        : propagation_timeout_(propagation_timeout)
    {}

    std::string getName() const override {
        return "DL2: Block Propagation";
    }

protected:
    std::vector<LivenessViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint64_t propagation_timeout_;

    /**
     * Get time when block was broadcast/mined
     */
    std::optional<uint64_t> getBlockBroadcastTime(
        const ConsensusTrace& trace,
        const std::string& block_hash
    ) const;

    /**
     * Get time when node received block
     */
    std::optional<uint64_t> getBlockReceivedTime(
        const ConsensusTrace& trace,
        const NodeID& node_id,
        const std::string& block_hash
    ) const;

    /**
     * Find nodes that didn't receive block by deadline
     */
    std::vector<NodeID> findNodesWithoutBlock(
        const ConsensusTrace& trace,
        const std::string& block_hash,
        uint64_t deadline,
        const std::vector<NodeID>& honest_nodes
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
