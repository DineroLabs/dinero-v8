#pragma once

#include "consensus_liveness_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DL5Oracle - Sync Completion Property
 *
 * Property: New nodes joining the network complete Initial Block Download
 *           (IBD) and reach the current chain tip within bounded time
 *
 * Sync Definition:
 * - Node starts at height 0 (or behind)
 * - "Current tip" = max height of established nodes
 * - Node reaches tip within sync_timeout
 * - Tolerates new blocks arriving during sync
 *
 * Violation Detection:
 * - Node starts syncing at T_start
 * - Target height = max height at T_start
 * - By T_start + sync_timeout, check if node reached target
 * - If not, report sync failure
 *
 * Why DL5 Matters:
 * - Ensures network accessibility (new nodes can join)
 * - Prevents IBD stalls
 * - Validates P2P sync protocol correctness
 * - Foundation for decentralization (anyone can sync)
 *
 * Example Scenario (Violation):
 * - Network at height 100
 * - Dave joins at T=0 (height 0)
 * - Target: reach height 100 by T=5000 (timeout=5000ms)
 * - Dave stuck at height 50 (slow peer, bugs)
 * - T=5000: Dave at height 50, not 100
 * - DL5 violation: IBD didn't complete
 *
 * Example Scenario (No Violation):
 * - Network at height 100
 * - Dave joins at T=0 (height 0)
 * - Dave syncs: height 0 → 50 → 100
 * - T=3000: Dave reaches height 100
 * - No violation: Synced in 3000ms < 5000ms timeout
 *
 * Timeout Considerations:
 * - Depends on chain length and block size
 * - Dinero regtest: small chain, fast sync
 * - Default: 5000ms (generous for small chains)
 *
 * Phase 5c Scope:
 * - Simplified: Check if nodes converged to same height
 * - Full implementation: Track IBD progress events
 */
class DL5Oracle : public ConsensusLivenessOracle {
public:
    /**
     * Create DL5 oracle
     *
     * @param sync_timeout Max time for new node to reach tip (ms)
     */
    explicit DL5Oracle(uint64_t sync_timeout = 5000)
        : sync_timeout_(sync_timeout)
    {}

    std::string getName() const override {
        return "DL5: Sync Completion";
    }

protected:
    std::vector<LivenessViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint64_t sync_timeout_;

    /**
     * Get time when node started (joined network)
     */
    std::optional<uint64_t> getNodeStartTime(
        const ConsensusTrace& trace,
        const NodeID& node_id
    ) const;

    /**
     * Get maximum height across all nodes at timestamp
     */
    uint32_t getNetworkHeightAtTime(
        const ConsensusTrace& trace,
        uint64_t timestamp
    ) const;

    /**
     * Get node's height at timestamp
     */
    std::optional<uint32_t> getNodeHeightAtTime(
        const ConsensusTrace& trace,
        const NodeID& node_id,
        uint64_t timestamp
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
