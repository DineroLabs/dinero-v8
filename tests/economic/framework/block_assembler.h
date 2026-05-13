#pragma once

#include "economic_types.h"
#include "mempool_simulator.h"
#include <vector>
#include <memory>
#include <optional>

namespace dinero {
namespace economic {
namespace test {

/**
 * BlockAssembler - Assembles block templates with economic optimization
 *
 * Selects transactions from mempool to maximize block fees while
 * respecting block size limits and consensus rules.
 *
 * Pattern: Observable-facts-only
 * - Template contents are observable (included txs, total fees)
 * - Selection algorithm is deterministic (same mempool → same template)
 * - No market prediction, just greedy fee maximization
 *
 * Integration:
 * - Uses MempoolSimulator to get candidate transactions
 * - Produces BlockTemplateState for EconomicTrace
 * - Works with EconomicSimulator for block mining
 *
 * Simplified for Phase 6a:
 * - Greedy selection (highest fee rate first)
 * - No ancestor/descendant tracking (CPFP in Phase 6c)
 * - No package relay (Phase 6c)
 * - Focus on fee-based prioritization
 */
class BlockAssembler {
public:
    /**
     * Create block assembler
     *
     * @param policy Economic policy (block size limits, etc.)
     */
    explicit BlockAssembler(const EconomicPolicy& policy);

    /**
     * Assemble block template from mempool
     *
     * Selects transactions greedily by fee rate until block is full.
     *
     * @param mempool Mempool to select from
     * @param chain_height Height of new block
     * @param timestamp Template creation time
     * @return Block template state
     */
    BlockTemplateState assembleTemplate(
        const MempoolSimulator& mempool,
        uint32_t chain_height,
        uint64_t timestamp
    );

    /**
     * Assemble template with specific transaction requirements
     *
     * Useful for testing scenarios where certain txs must be included
     *
     * @param mempool Mempool to select from
     * @param required_txs Transactions that must be included
     * @param chain_height Height of new block
     * @param timestamp Template creation time
     * @return Block template state
     */
    BlockTemplateState assembleTemplateWithRequirements(
        const MempoolSimulator& mempool,
        const std::vector<TxID>& required_txs,
        uint32_t chain_height,
        uint64_t timestamp
    );

    /**
     * Get events from last template assembly
     *
     * Includes:
     * - TX_SELECTED_FOR_BLOCK for each included tx
     * - TX_EXCLUDED_FROM_BLOCK for each skipped tx
     * - BLOCK_TEMPLATE_ASSEMBLED when complete
     *
     * @return Events describing template assembly
     */
    std::vector<EconomicEvent> getAssemblyEvents() const {
        return assembly_events_;
    }

    /**
     * Clear assembly events (for next template)
     */
    void clearEvents() {
        assembly_events_.clear();
    }

    /**
     * Get current policy
     */
    const EconomicPolicy& getPolicy() const {
        return policy_;
    }

    /**
     * Update block size limit
     */
    void setMaxBlockSize(uint32_t max_size_bytes) {
        policy_.max_block_size_bytes = max_size_bytes;
    }

private:
    EconomicPolicy policy_;

    // Events from last template assembly
    std::vector<EconomicEvent> assembly_events_;
    uint64_t event_sequence_;

    /**
     * Select transactions using greedy algorithm
     *
     * Sort by fee rate, add highest fee rate first until block full
     *
     * @param candidates Available transactions
     * @param max_size Maximum block size
     * @return Selected transactions
     */
    std::vector<MempoolEntry> greedySelect(
        const std::vector<MempoolEntry>& candidates,
        uint32_t max_size
    );

    /**
     * Check if transaction fits in remaining space
     */
    bool fits(uint32_t current_size, uint32_t tx_size, uint32_t max_size) const;

    /**
     * Record event
     */
    void recordEvent(EconomicEvent event);

    /**
     * Create template hash (deterministic based on contents)
     */
    BlockHash createTemplateHash(
        const std::vector<TxID>& tx_ids,
        uint32_t height,
        uint64_t timestamp
    ) const;
};

} // namespace test
} // namespace economic
} // namespace dinero
