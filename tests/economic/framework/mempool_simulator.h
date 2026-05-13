#pragma once

#include "economic_types.h"
#include "fee_estimator.h"
#include <vector>
#include <map>
#include <optional>
#include <memory>

namespace dinero {
namespace economic {
namespace test {

/**
 * MempoolSimulator - Simulates mempool economics for a single node
 *
 * Manages transaction pool with economic policies:
 * - Fee validation (minimum relay fee, dust threshold)
 * - RBF (replace-by-fee) handling
 * - Mempool eviction (when full)
 * - Transaction prioritization (by fee rate)
 *
 * Pattern: Observable-facts-only
 * - Mempool state is observable (transactions, fees, size)
 * - Fee calculations are deterministic
 * - No market prediction, just policy enforcement
 *
 * Integration:
 * - Used by EconomicSimulator for each node
 * - Provides economic state to EconomicTrace
 * - Works with FeeEstimator for fee estimation
 */
class MempoolSimulator {
public:
    /**
     * Create mempool simulator for node
     *
     * @param node_id Node identifier
     * @param policy Economic policy configuration
     * @param fee_estimator Fee estimator (shared across nodes or per-node)
     */
    MempoolSimulator(
        const NodeID& node_id,
        const EconomicPolicy& policy,
        std::shared_ptr<FeeEstimator> fee_estimator
    );

    // ========================================================================
    // Transaction Submission
    // ========================================================================

    /**
     * Submit transaction to mempool
     *
     * Validates fees, checks for conflicts, applies RBF rules
     *
     * @param entry Transaction to submit
     * @param timestamp Submission time
     * @return Event describing outcome (accepted/rejected)
     */
    EconomicEvent submitTransaction(const MempoolEntry& entry, uint64_t timestamp);

    /**
     * Replace transaction with higher-fee version (RBF)
     *
     * @param old_tx_id Transaction to replace
     * @param new_entry Replacement transaction
     * @param timestamp Replacement time
     * @return Event describing outcome
     */
    EconomicEvent replaceTransaction(
        const TxID& old_tx_id,
        const MempoolEntry& new_entry,
        uint64_t timestamp
    );

    // ========================================================================
    // Mempool Queries
    // ========================================================================

    /**
     * Check if transaction is in mempool
     */
    bool hasTx(const TxID& tx_id) const;

    /**
     * Get transaction from mempool
     */
    std::optional<MempoolEntry> getTx(const TxID& tx_id) const;

    /**
     * Get all transactions in mempool
     */
    std::vector<MempoolEntry> getAllTxs() const;

    /**
     * Get transactions sorted by fee rate (highest first)
     */
    std::vector<MempoolEntry> getTxsSortedByFeeRate() const;

    /**
     * Get mempool size in bytes
     */
    uint64_t getSize() const;

    /**
     * Get transaction count
     */
    size_t getTxCount() const {
        return mempool_.size();
    }

    /**
     * Get total fees in mempool
     */
    uint64_t getTotalFees() const;

    // ========================================================================
    // Mempool Management
    // ========================================================================

    /**
     * Remove transaction from mempool
     *
     * Used when tx is confirmed in block or evicted
     *
     * @param tx_id Transaction to remove
     * @return Event describing removal, or nullopt if not in mempool
     */
    std::optional<EconomicEvent> removeTx(const TxID& tx_id, uint64_t timestamp);

    /**
     * Remove multiple transactions (e.g., confirmed in block)
     */
    std::vector<EconomicEvent> removeTxs(const std::vector<TxID>& tx_ids, uint64_t timestamp);

    /**
     * Evict low-fee transactions to make room
     *
     * Called when mempool is full and new tx arrives
     *
     * @param required_bytes Space needed
     * @param timestamp Eviction time
     * @return List of evicted transactions
     */
    std::vector<EconomicEvent> evictLowFeeTxs(uint64_t required_bytes, uint64_t timestamp);

    /**
     * Clear all transactions from mempool
     */
    void clear();

    // ========================================================================
    // Fee Validation
    // ========================================================================

    /**
     * Validate transaction fees against policy
     *
     * Checks:
     * - Minimum relay fee
     * - Dust outputs
     * - Fee overflow
     * - Value conservation (outputs <= inputs)
     *
     * @return nullopt if valid, error message if invalid
     */
    std::optional<std::string> validateFees(const MempoolEntry& entry) const;

    /**
     * Check if RBF replacement is valid
     *
     * BIP 125 rules (simplified):
     * - New fee rate must be higher
     * - Absolute fee must increase by minimum amount
     * - No new unconfirmed inputs
     *
     * @return nullopt if valid, error message if invalid
     */
    std::optional<std::string> validateRBF(
        const MempoolEntry& old_entry,
        const MempoolEntry& new_entry
    ) const;

    // ========================================================================
    // State Capture
    // ========================================================================

    /**
     * Capture current economic state (for EconomicTrace)
     */
    EconomicState captureState(uint64_t timestamp, uint32_t chain_height) const;

    // ========================================================================
    // Policy Configuration
    // ========================================================================

    /**
     * Update minimum relay fee
     */
    void setMinRelayFee(uint64_t fee_una);

    /**
     * Update dust threshold
     */
    void setDustThreshold(uint64_t threshold_una);

    /**
     * Get current policy
     */
    const EconomicPolicy& getPolicy() const {
        return policy_;
    }

    // ========================================================================
    // Getters
    // ========================================================================

    const NodeID& getNodeId() const {
        return node_id_;
    }

private:
    NodeID node_id_;
    EconomicPolicy policy_;
    std::shared_ptr<FeeEstimator> fee_estimator_;

    // Mempool storage: tx_id → entry
    std::map<TxID, MempoolEntry> mempool_;

    // Sequence counter for events
    uint64_t event_sequence_;

    /**
     * Check if mempool has space for new transaction
     */
    bool hasSpace(uint32_t tx_size_bytes) const;

    /**
     * Get current mempool size in bytes
     */
    uint64_t getCurrentSize() const;

    /**
     * Create economic event
     */
    EconomicEvent createEvent(
        EconomicEventType type,
        uint64_t timestamp,
        bool success,
        const std::string& error_message = "",
        const std::optional<TxID>& tx_id = std::nullopt,
        const std::optional<MempoolEntry>& entry = std::nullopt
    );
};

} // namespace test
} // namespace economic
} // namespace dinero
