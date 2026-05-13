#pragma once

#include "economic_types.h"
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace dinero {
namespace economic {
namespace test {

/**
 * EconomicTrace - Complete execution history for economic simulation
 *
 * Analogous to ConsensusTrace from Ring 5, but focused on economic properties:
 * fee validation, mempool management, block assembly, and incentive alignment.
 *
 * Records all economic actions (inputs), events (outputs), and state snapshots
 * across all nodes in the network.
 *
 * Pattern: Simulator executes actions → generates events → captures state snapshots
 * Oracle: Reads trace → observes events → reports violations
 */
struct EconomicTrace {
    // ========================================================================
    // Configuration
    // ========================================================================

    uint64_t rng_seed;              // For deterministic reproducibility
    std::string scenario_name;       // Test identification (e.g., "RBF replacement")
    std::vector<NodeID> nodes;       // Participating nodes
    EconomicPolicy policy;           // Economic policy configuration

    // ========================================================================
    // Execution History (Global Timeline)
    // ========================================================================

    // Actions: What we commanded the system to do
    std::vector<EconomicAction> actions;

    // Events: What actually happened (per-node outputs)
    std::vector<EconomicEvent> events;

    // State snapshots: Periodic checkpoints of all node economic states
    std::vector<EconomicState> snapshots;

    // ========================================================================
    // Determinism Verification
    // ========================================================================

    uint64_t final_hash;             // Hash of entire trace (for E21-E25 oracles)

    // ========================================================================
    // Metadata
    // ========================================================================

    uint64_t start_time;             // Simulation start timestamp (ms)
    uint64_t end_time;               // Simulation end timestamp (ms)
    bool completed_successfully;     // Did simulation complete without errors?
    std::optional<std::string> failure_reason;  // Error details if failed

    // ========================================================================
    // Economic Metrics (Aggregate)
    // ========================================================================

    // Transaction metrics
    uint64_t total_txs_submitted;
    uint64_t total_txs_accepted;
    uint64_t total_txs_rejected;
    uint64_t total_txs_confirmed;

    // Fee metrics
    uint64_t total_fees_collected;   // Sum of all fees in confirmed blocks
    uint64_t total_fees_rejected;    // Sum of fees in rejected txs

    // Mempool metrics
    uint64_t max_mempool_size_bytes; // Peak mempool size
    uint64_t max_mempool_tx_count;   // Peak tx count

    // Block assembly metrics
    uint64_t total_blocks_mined;
    uint64_t total_block_fees;       // Sum of fees across all blocks

    // ========================================================================
    // Methods
    // ========================================================================

    /**
     * Compute deterministic hash of entire trace
     *
     * Hash includes:
     * - RNG seed
     * - All actions (type, timestamp, sequence, tx_id, fee)
     * - All events (type, timestamp, sequence, node_id, success, fee)
     * - All state snapshots (node_id, timestamp, mempool state, fees)
     *
     * Used by E21-E25 oracles to verify economic determinism.
     */
    uint64_t computeHash() const;

    /**
     * Get all events for a specific node
     */
    std::vector<EconomicEvent> getEventsForNode(const NodeID& node_id) const;

    /**
     * Get all state snapshots for a specific node
     */
    std::vector<EconomicState> getSnapshotsForNode(const NodeID& node_id) const;

    /**
     * Get state snapshot at specific timestamp (closest snapshot before timestamp)
     */
    std::optional<EconomicState> getStateAt(const NodeID& node_id, uint64_t timestamp) const;

    /**
     * Get all transactions that were submitted during trace
     */
    std::vector<TxID> getAllSubmittedTxs() const;

    /**
     * Get all transactions that were confirmed in blocks
     */
    std::vector<TxID> getAllConfirmedTxs() const;

    /**
     * Get all transactions currently in mempool at end of trace
     */
    std::vector<TxID> getFinalMempoolTxs() const;

    /**
     * Get events of specific type
     */
    std::vector<EconomicEvent> getEventsByType(EconomicEventType type) const;

    /**
     * Get events for specific transaction
     */
    std::vector<EconomicEvent> getEventsForTx(const TxID& tx_id) const;

    /**
     * Get total fees paid by a specific transaction (if confirmed)
     */
    std::optional<uint64_t> getTxFee(const TxID& tx_id) const;

    /**
     * Get simulation duration (end_time - start_time)
     */
    uint64_t getDuration() const {
        return end_time - start_time;
    }

    /**
     * Get total number of events across all nodes
     */
    size_t getTotalEventCount() const {
        return events.size();
    }

    /**
     * Get total number of snapshots across all nodes
     */
    size_t getTotalSnapshotCount() const {
        return snapshots.size();
    }

    /**
     * Check if a transaction was confirmed in a block
     */
    bool isTxConfirmed(const TxID& tx_id) const;

    /**
     * Check if a transaction is in mempool at end of trace
     */
    bool isTxInMempool(const TxID& tx_id) const;

    /**
     * Get average fee rate across all confirmed transactions
     */
    double getAverageFeeRate() const;

    /**
     * Get total mempool fees at end of trace
     */
    uint64_t getFinalMempoolFees() const;
};

} // namespace test
} // namespace economic
} // namespace dinero
