#pragma once

#include "consensus_liveness_oracle.h"

namespace dinero {
namespace consensus {
namespace test {

/**
 * DL4Oracle - Transaction Inclusion Property
 *
 * Property: Valid transactions broadcast to the network eventually
 *           get included in accepted blocks
 *
 * Inclusion Definition:
 * - Transaction broadcast at time T_broadcast
 * - Transaction appears in accepted block by T_broadcast + timeout
 * - "Valid" = passes validation, non-conflicting
 *
 * Violation Detection:
 * - Transaction broadcast at T_broadcast
 * - No BLOCK_ACCEPTED event contains this tx by deadline
 * - Report inclusion failure
 *
 * Why DL4 Matters:
 * - Ensures censorship resistance
 * - Guarantees transaction finality path
 * - Prevents mempool starvation
 * - Foundation for user expectations (tx will confirm)
 *
 * Example Scenario (Violation):
 * - Alice broadcasts tx_1 at T=100
 * - Miners ignore tx_1 (censorship or bug)
 * - By T=3100 (timeout=3000ms), tx_1 not in any block
 * - DL4 violation: tx_1 failed to be included
 *
 * Example Scenario (No Violation):
 * - Alice broadcasts tx_1 at T=100
 * - Bob mines block_1 at T=500, includes tx_1
 * - Block_1 accepted by network
 * - No violation: tx_1 included within 400ms
 *
 * Timeout Considerations:
 * - Must account for block time + propagation
 * - Default: 3000ms (1-2 blocks in regtest + margin)
 * - Real Bitcoin: ~10min (1 block)
 *
 * Phase 5c Scope:
 * - Simplified: Track TX_RECEIVED events
 * - Full implementation: Parse block contents for txs
 */
class DL4Oracle : public ConsensusLivenessOracle {
public:
    /**
     * Create DL4 oracle
     *
     * @param inclusion_timeout Max time for tx to be included (ms)
     */
    explicit DL4Oracle(uint64_t inclusion_timeout = 3000)
        : inclusion_timeout_(inclusion_timeout)
    {}

    std::string getName() const override {
        return "DL4: Transaction Inclusion";
    }

protected:
    std::vector<LivenessViolation> observeTrace(const ConsensusTrace& trace) override;

private:
    uint64_t inclusion_timeout_;

    /**
     * Get time when transaction was first broadcast/received
     */
    std::optional<uint64_t> getTxBroadcastTime(
        const ConsensusTrace& trace,
        const std::string& tx_hash
    ) const;

    /**
     * Check if transaction was included in any accepted block
     */
    bool wasTxIncluded(
        const ConsensusTrace& trace,
        const std::string& tx_hash,
        uint64_t deadline
    ) const;

    /**
     * Get all unique transactions from trace
     */
    std::vector<std::string> getAllTransactions(const ConsensusTrace& trace) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
