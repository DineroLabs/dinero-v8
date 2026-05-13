#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <map>

namespace dinero {
namespace economic {
namespace test {

// ============================================================================
// Node Identification (reuse from consensus layer)
// ============================================================================

using NodeID = std::string;
using TxID = std::string;
using BlockHash = std::string;

// ============================================================================
// Action Types (Inputs to the economic simulator - what we command)
// ============================================================================

enum class EconomicActionType {
    // Transaction actions
    SUBMIT_TX,               // Submit transaction with fees
    REPLACE_TX_RBF,          // Replace-by-fee transaction replacement
    BROADCAST_PACKAGE,       // CPFP package submission
    EVICT_TX,                // Force mempool eviction

    // Fee policy actions
    SET_MIN_RELAY_FEE,       // Update minimum relay fee
    SET_DUST_THRESHOLD,      // Update dust threshold
    UPDATE_FEE_ESTIMATE,     // Trigger fee estimator update

    // Mining/block assembly actions
    REQUEST_BLOCK_TEMPLATE,  // Request mining template
    MINE_BLOCK,              // Mine block with template
    SUBMIT_BLOCK,            // Submit mined block

    // Economic attack actions
    SPAM_MEMPOOL,            // Submit many low-fee txs
    FEE_SNIPE_ATTEMPT,       // Attempt fee sniping
    PIN_TRANSACTION,         // Transaction pinning attack
    FREE_RELAY_ATTEMPT,      // Attempt free relay

    // Time actions
    ADVANCE_TIME             // Tick global clock
};

struct EconomicAction {
    EconomicActionType type;
    uint64_t timestamp;           // When this action occurs
    uint64_t sequence_number;     // Global action ordering

    // Action-specific data
    std::optional<NodeID> node_id;          // Which node (if applicable)
    std::optional<TxID> tx_id;              // Transaction identifier
    std::optional<uint64_t> fee_una;   // Transaction fee
    std::optional<uint64_t> input_value;    // Total input value
    std::optional<uint64_t> output_value;   // Total output value
    std::optional<uint32_t> tx_size_bytes;  // Transaction size
    std::optional<double> fee_rate;         // sat/byte or sat/vbyte

    // RBF-specific
    std::optional<TxID> replaces_tx_id;     // For RBF
    std::optional<uint64_t> rbf_fee_delta;  // Fee increase for RBF

    // Package-specific
    std::optional<std::vector<TxID>> package_txs;  // For CPFP packages

    // Policy updates
    std::optional<uint64_t> min_relay_fee_una;
    std::optional<uint64_t> dust_threshold_una;

    // Attack parameters
    std::optional<uint32_t> spam_count;     // Number of spam txs
    std::optional<std::string> attack_strategy;

    // Time delta
    std::optional<uint64_t> time_delta_ms;  // For ADVANCE_TIME
};

// ============================================================================
// Event Types (Outputs from the economic simulator - what happened)
// ============================================================================

enum class EconomicEventType {
    // Transaction lifecycle events
    TX_SUBMITTED,            // Transaction submitted to node
    TX_ACCEPTED_TO_MEMPOOL,  // TX accepted into mempool
    TX_REJECTED_LOW_FEE,     // TX rejected (fee too low)
    TX_REJECTED_DUST,        // TX rejected (dust output)
    TX_REJECTED_INVALID,     // TX rejected (consensus invalid)
    TX_REPLACED_RBF,         // TX replaced by higher-fee version
    TX_EVICTED_MEMPOOL,      // TX evicted from mempool
    TX_INCLUDED_IN_BLOCK,    // TX confirmed in block
    TX_REORGED_OUT,          // TX reverted due to reorg

    // Fee calculation events
    FEE_CALCULATED,          // Fee amount calculated
    FEE_VALIDATED,           // Fee validation result
    FEE_OVERFLOW_DETECTED,   // Fee calculation overflow

    // Mempool events
    MEMPOOL_SIZE_CHANGED,    // Mempool size updated
    MEMPOOL_FULL,            // Mempool at capacity
    MEMPOOL_EVICTION,        // Low-fee txs evicted

    // Block assembly events
    BLOCK_TEMPLATE_REQUESTED,   // Template generation started
    BLOCK_TEMPLATE_ASSEMBLED,   // Template created
    TX_SELECTED_FOR_BLOCK,      // TX included in template
    TX_EXCLUDED_FROM_BLOCK,     // TX skipped (low fee, conflict, etc.)

    // Fee estimation events
    FEE_ESTIMATE_UPDATED,    // Fee estimator updated
    FEE_ESTIMATE_QUERIED,    // Fee estimate requested

    // Economic attack detection
    SPAM_DETECTED,           // Spam pattern detected
    FREE_RELAY_DETECTED,     // Free relay attempt detected
    PINNING_DETECTED,        // Transaction pinning detected

    // Relay events
    TX_RELAYED,              // TX forwarded to peers
    TX_RELAY_REJECTED        // TX not relayed (policy)
};

struct EconomicEvent {
    EconomicEventType type;
    uint64_t timestamp;           // When this event occurred
    uint64_t sequence_number;     // Global event ordering
    NodeID node_id;               // Which node generated this event

    // Event-specific data
    std::optional<TxID> tx_id;
    std::optional<BlockHash> block_hash;
    std::optional<uint32_t> block_height;

    // Fee information
    std::optional<uint64_t> fee_una;
    std::optional<uint64_t> input_value;
    std::optional<uint64_t> output_value;
    std::optional<uint32_t> tx_size_bytes;
    std::optional<double> fee_rate;         // sat/byte

    // Mempool information
    std::optional<size_t> mempool_tx_count;
    std::optional<uint64_t> mempool_size_bytes;
    std::optional<uint64_t> mempool_total_fees;

    // Template information
    std::optional<std::vector<TxID>> template_txs;
    std::optional<uint64_t> template_total_fees;

    // Fee estimate information
    std::optional<double> estimated_fee_rate;
    std::optional<uint32_t> confirmation_target;

    // RBF information
    std::optional<TxID> replaced_tx_id;
    std::optional<uint64_t> fee_delta;

    bool success;                // Event outcome (e.g., validation result)
    std::string error_message;   // Error details (if success=false)
    std::string details;         // Additional context
};

// ============================================================================
// State Types (Economic state snapshots)
// ============================================================================

struct MempoolEntry {
    TxID tx_id;
    uint64_t fee_una;
    uint64_t input_value;
    uint64_t output_value;
    uint32_t tx_size_bytes;
    double fee_rate;             // sat/byte
    uint64_t entry_time;         // When tx entered mempool

    // RBF state
    bool signals_rbf;
    std::optional<TxID> replaces_tx_id;

    // Ancestry/descendants (for CPFP)
    std::vector<TxID> parent_txs;
    std::vector<TxID> child_txs;
};

struct BlockTemplateState {
    BlockHash template_hash;
    uint32_t height;
    std::vector<TxID> included_txs;
    uint64_t total_fees;
    uint32_t total_size_bytes;
    uint64_t creation_time;
};

struct FeeEstimate {
    uint32_t confirmation_target;  // Blocks
    double estimated_fee_rate;     // sat/byte
    uint64_t timestamp;
};

struct EconomicState {
    NodeID node_id;
    uint64_t timestamp;

    // Mempool state
    std::vector<MempoolEntry> mempool_entries;
    size_t mempool_tx_count;
    uint64_t mempool_size_bytes;
    uint64_t mempool_total_fees;

    // Block template state
    std::optional<BlockTemplateState> current_template;

    // Fee policy state
    uint64_t min_relay_fee_una;
    uint64_t dust_threshold_una;

    // Fee estimation state
    std::vector<FeeEstimate> fee_estimates;

    // Chain state (from consensus layer)
    BlockHash chain_tip_hash;
    uint32_t chain_height;

    // Economic metrics
    uint64_t total_fees_collected;     // Lifetime
    uint64_t total_txs_confirmed;      // Lifetime
    uint64_t total_txs_rejected;       // Lifetime

    // Attack detection state
    uint32_t spam_tx_count;            // Recent spam count
    uint32_t free_relay_attempts;      // Detected free relay attempts
};

// ============================================================================
// Economic Policy Configuration
// ============================================================================

struct EconomicPolicy {
    // Fee policy
    uint64_t min_relay_fee_una = 1000;      // Default: 1000 sat
    uint64_t dust_threshold_una = 546;      // Default: 546 sat (P2PKH)

    // Mempool policy
    uint64_t max_mempool_size_bytes = 300 * 1024 * 1024;  // 300 MB
    uint32_t max_mempool_tx_count = 100000;
    uint64_t mempool_expiry_hours = 336;         // 14 days

    // RBF policy
    bool enable_rbf = true;
    double rbf_min_fee_increment = 1.0;          // Must increase fee rate by 1 sat/byte
    uint64_t rbf_min_absolute_fee = 1000;        // Minimum absolute fee increase

    // Block assembly policy
    uint32_t max_block_size_bytes = 1000000;     // 1 MB (legacy)
    uint64_t min_block_tx_fee = 0;               // No minimum for inclusion

    // Fee estimation policy
    std::vector<uint32_t> confirmation_targets = {1, 3, 6, 12, 24};  // Blocks

    // Attack resistance
    uint32_t spam_detection_threshold = 100;     // Txs per hour
    uint64_t free_relay_min_fee = 1000;          // Minimum fee to relay
};

} // namespace test
} // namespace economic
} // namespace dinero
