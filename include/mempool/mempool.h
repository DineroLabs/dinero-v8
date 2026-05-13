#pragma once

#include "consensus/outpoint.h"
#include "primitives/uint256.h"
#include "primitives/transaction.h"
#include "consensus/tx_validation.h"
#include "consensus/chain_state_view.h"
#include "common/status.h"
#include "mempool/invalid_tx_cache.h"
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace dinero {
namespace mempool {

// ============================================================================
// Phase 25: Mempool (Transaction Pool)
// ============================================================================

/**
 * Mempool entry - A transaction in the mempool with metadata
 *
 * Phase M.0: Migrated to uint256 for transaction identity
 *
 * Tracks all information needed for fee estimation, eviction, and mining:
 * - Transaction data
 * - Fee information (absolute fee, fee rate, ancestor fees)
 * - Dependency tracking (parents, children)
 * - Timing information (when added)
 * - Size metrics (virtual size for SegWit)
 */
struct MempoolEntry {
    // Transaction data
    Transaction tx;
    TxId txid;  // Phase M.4.3-C: Semantic type safety (malleability-proof)

    // Helper: Get txid as hex (RPC boundary only)
    std::string GetTxIdHex() const { return txid.AsUint256().GetHex(); }

    // Fee information
    uint64_t fee;              // Absolute fee in una
    uint64_t base_fee;         // Fee without descendants
    size_t size;               // Transaction size in bytes
    size_t vsize;              // Virtual size (for SegWit weight calculation)
    double fee_rate;           // Fee per vbyte (sat/vB)

    // Ancestor tracking (for CPFP - Child Pays For Parent)
    uint64_t ancestor_fee;     // Total fees of all ancestors + this tx
    size_t ancestor_size;      // Total size of all ancestors + this tx
    size_t ancestor_count;     // Number of ancestor transactions

    // Descendant tracking
    uint64_t descendant_fee;   // Total fees of all descendants + this tx
    size_t descendant_size;    // Total size of all descendants + this tx
    size_t descendant_count;   // Number of descendant transactions

    // Dependency graph (Phase M.4.3-C: Type-safe TxId)
    std::unordered_set<TxId> parents;     // Transactions this depends on (direct parents only)
    std::unordered_set<TxId> children;    // Transactions depending on this (direct children only)

    // F.9.6: Cached ancestor set (computed ONCE at admission)
    // This is the KEY to O(A) complexity - never recompute by traversal
    // Phase M.4.3-C: Type-safe TxId (cannot be WTxId)
    std::vector<TxId> ancestors;          // ALL ancestors (parents, grandparents, etc.) - sorted for determinism

    // Metadata
    uint64_t time_added;       // Timestamp when added to mempool
    uint32_t height;           // Block height when added

    // RBF (Replace-By-Fee)
    bool signals_rbf;          // BIP 125: Signals replacement

    // Phase C.2: Covenant tracking (in-memory only, not persisted)
    bool has_covenant_input;   // Does this tx spend any covenant-locked UTXO?
    uint32_t covenant_count;   // Number of covenant inputs (for policy limits)

    MempoolEntry()
        : fee(0), base_fee(0), size(0), vsize(0), fee_rate(0.0)
        , ancestor_fee(0), ancestor_size(0), ancestor_count(0)
        , descendant_fee(0), descendant_size(0), descendant_count(0)
        , time_added(0), height(0), signals_rbf(false)
        , has_covenant_input(false), covenant_count(0)
    {}
};

/**
 * Mempool acceptance result
 */
enum class MempoolAcceptResult {
    OK = 0,
    ALREADY_IN_MEMPOOL,
    ALREADY_IN_CHAIN,
    INVALID_TX,              // Failed consensus validation
    INSUFFICIENT_FEE,        // Fee too low
    MEMPOOL_FULL,            // Mempool at capacity
    CONFLICTS_WITH_MEMPOOL,  // Double spend without RBF
    RBF_REJECTED,            // RBF attempted but failed rules
    TOO_MANY_ANCESTORS,      // Exceeds ancestor limit
    TOO_MANY_DESCENDANTS,    // Exceeds descendant limit
    MISSING_INPUTS,          // Parent transactions not in mempool or UTXO
    SCRIPT_VERIFY_FAILED,    // Script validation failed
    LOCKTIME_NOT_SATISFIED,  // Transaction not yet valid

    // Phase C.2: Covenant-specific rejections (policy-only, not consensus)
    COVENANT_ANCESTOR_MISSING,   // Covenant parent not confirmed or in mempool
    COVENANT_RBF_FORBIDDEN,      // Cannot RBF covenant transactions (policy)
    TOO_MANY_COVENANT_INPUTS,    // Exceeds covenant input limit (DoS protection)
};

const char* MempoolAcceptResultToString(MempoolAcceptResult result);

/**
 * Mempool submission mode
 *
 * TEST_ONLY: Skip script/signature validation for policy testing
 * - Used by wallet.sendtoaddress with test_mode=true
 * - Enables mempool policy testing without Phase 34 (signing)
 * - Transactions marked TEST_ONLY are never relayed
 * - Only available in regtest mode
 */
enum class MempoolSubmitMode {
    NORMAL,      // Full validation (requires valid signatures)
    TEST_ONLY    // Skip script validation (policy testing only)
};

/**
 * Mempool configuration
 */
struct MempoolConfig {
    size_t max_size_mb;           // Maximum mempool size (default: 300 MB)
    double min_fee_rate;          // Minimum fee rate (sat/vB) (default: 1.0)
    size_t max_ancestors;         // Max ancestor count (default: 25)
    size_t max_descendants;       // Max descendant count (default: 25)
    size_t max_ancestor_size_kb;  // Max ancestor size (default: 101 KB)
    bool enable_rbf;              // Enable Replace-By-Fee (default: false)
    uint64_t expiry_hours;        // Transaction expiry time (default: 336 = 2 weeks)

    // Phase C.2: Covenant policy limits (DoS protection)
    size_t max_covenant_inputs_per_tx;  // Max covenant inputs in one tx (default: 10)
    bool allow_covenant_rbf;            // Allow RBF for covenant txs (default: false - conservative)

    // Phase E.2.a: Validation scratch space limits (DoS protection)
    size_t max_validation_memory_mb;    // Max memory for single tx validation (default: 50 MB)
    size_t max_script_stack_bytes;      // Max script stack size (default: 10 MB)
    size_t max_signature_cache_mb;      // Max signature verification cache (default: 100 MB)

    MempoolConfig()
        : max_size_mb(300)
        , min_fee_rate(1.0)
        , max_ancestors(25)
        , max_descendants(25)
        , max_ancestor_size_kb(101)
        , enable_rbf(false)
        , expiry_hours(336)
        , max_covenant_inputs_per_tx(10)
        , allow_covenant_rbf(false)
        , max_validation_memory_mb(50)
        , max_script_stack_bytes(10 * 1024 * 1024)
        , max_signature_cache_mb(100)
    {}
};

/**
 * Transaction Mempool
 *
 * The mempool stores unconfirmed transactions waiting to be mined.
 * It provides:
 * - Transaction validation and acceptance
 * - Fee estimation and prioritization
 * - Conflict detection (double spends)
 * - RBF (Replace-By-Fee) support
 * - CPFP (Child Pays For Parent) via ancestor tracking
 * - Eviction when full
 * - Block template building support
 */
class Mempool {
public:
    explicit Mempool(const MempoolConfig& config = MempoolConfig());
    ~Mempool();

    // ========================================================================
    // Transaction Acceptance
    // ========================================================================

    /**
     * Accept transaction into mempool
     *
     * Performs full validation:
     * 1. Consensus validation (script, inputs, outputs)
     * 2. Fee validation (meets minimum)
     * 3. Conflict detection (double spends)
     * 4. Ancestor/descendant limits
     * 5. RBF rules (if replacing)
     *
     * @param tx             Transaction to add
     * @param coins_view     UTXO view (with mempool modifications)
     * @param current_height Current block height
     * @param current_time   Current median time past
     * @return               Acceptance result
     */
    MempoolAcceptResult acceptTransaction(
        const Transaction& tx,
        const consensus::ChainStateView& coins_view,
        uint32_t current_height,
        uint64_t current_time
    );

    /**
     * Submit transaction with mode control (TEST_ONLY for policy testing)
     *
     * @param tx             Transaction to add
     * @param coins_view     UTXO view (with mempool modifications)
     * @param current_height Current block height
     * @param current_time   Current median time past
     * @param mode           Submission mode (NORMAL or TEST_ONLY)
     * @return               Acceptance result
     */
    MempoolAcceptResult submitTransaction(
        const Transaction& tx,
        const consensus::ChainStateView& coins_view,
        uint32_t current_height,
        uint64_t current_time,
        MempoolSubmitMode mode
    );

    /**
     * Remove transaction from mempool
     * Phase M.4.3-C: Changed to TxId for type safety
     */
    bool removeTransaction(const TxId& txid, bool recursive = false);

    /**
     * Remove transactions that are now in a block
     */
    void removeForBlock(const std::vector<Transaction>& block_txs);

    // ========================================================================
    // Queries
    // ========================================================================

    /**
     * Check if transaction is in mempool
     * Phase M.4.3-C: Changed to TxId for type safety
     */
    bool contains(const TxId& txid) const;

    /**
     * Get transaction by ID
     * Phase M.4.3-C: Changed to TxId for type safety
     */
    std::shared_ptr<const MempoolEntry> getEntry(const TxId& txid) const;

    /**
     * Get all transactions (for block template)
     */
    std::vector<std::shared_ptr<const MempoolEntry>> getAllEntries() const;

    /**
     * Get transactions sorted by fee rate (descending)
     */
    std::vector<std::shared_ptr<const MempoolEntry>> getEntriesByFeeRate() const;

    /**
     * Get transactions sorted by ancestor score (for mining)
     */
    std::vector<std::shared_ptr<const MempoolEntry>> getEntriesByAncestorScore() const;

    /**
     * Phase 34: Get transactions for block assembly
     *
     * Returns transactions sorted by ancestor score (fee/size), suitable for
     * inclusion in a block. Respects weight limits and ensures topological
     * ordering (parents before children).
     *
     * @param max_weight   Maximum block weight (default: 4000000 WU)
     * @return             Ordered list of transactions ready for block inclusion
     */
    std::vector<std::shared_ptr<const MempoolEntry>> GetTransactionsForBlock(
        size_t max_weight = 4000000  // Bitcoin's MAX_BLOCK_WEIGHT
    ) const;

    /**
     * Get mempool size in bytes
     */
    size_t getSize() const;

    /**
     * Get number of transactions
     */
    size_t getCount() const;

    /**
     * Get total fees in mempool
     */
    uint64_t getTotalFees() const;

    // ========================================================================
    // Fee Estimation
    // ========================================================================

    /**
     * Estimate fee rate for confirmation in N blocks
     *
     * @param target_blocks  Number of blocks for confirmation
     * @return               Estimated fee rate (sat/vB)
     */
    double estimateFeeRate(size_t target_blocks) const;

    /**
     * Get minimum fee rate to enter mempool
     */
    double getMinFeeRate() const { return config_.min_fee_rate; }

    // ========================================================================
    // Maintenance
    // ========================================================================

    /**
     * F.9.7: Eviction statistics
     */
    struct EvictionStats {
        size_t expired_count;       // Transactions removed due to expiry
        uint64_t expired_fees;      // Total fees from expired transactions
        size_t size_evicted_count;  // Transactions removed due to size limit
        uint64_t size_evicted_fees; // Total fees from size-evicted transactions
    };

    /**
     * Evict low-fee transactions when mempool is full
     *
     * F.9.7: Eviction strategy:
     * 1. Remove expired transactions (> expiry_hours = 336h = 14 days)
     * 2. Remove lowest fee rate transactions if over size limit
     * 3. Recursive removal (removes descendants automatically)
     *
     * Returns statistics about evicted transactions for logging.
     */
    EvictionStats evictTransactions();

    /**
     * Clear all transactions
     */
    void clear();

    // ========================================================================
    // Reorg Handling
    // ========================================================================

    /**
     * Reconcile mempool after a blockchain reorganization
     *
     * When a reorg occurs:
     * 1. Transactions from disconnected blocks become unconfirmed
     * 2. These should be returned to mempool (if still valid)
     * 3. Transactions in new connected blocks are already removed via removeForBlock
     *
     * This method:
     * - Takes transactions from disconnected blocks
     * - Filters out any that are now in connected blocks (by txid)
     * - Re-validates remaining against the new UTXO set
     * - Re-adds valid transactions to mempool
     *
     * @param disconnected_txs   Transactions from disconnected blocks
     * @param connected_txs      Transactions from newly connected blocks (to filter)
     * @param coins_view         Current UTXO view (post-reorg)
     * @param current_height     Current chain height (post-reorg)
     * @param current_time       Current median time past
     * @return                   Number of transactions successfully restored
     */
    size_t ReconcileAfterReorg(
        const std::vector<Transaction>& disconnected_txs,
        const std::vector<Transaction>& connected_txs,
        const consensus::ChainStateView& coins_view,
        uint32_t current_height,
        uint64_t current_time
    );

    /**
     * Phase E.2.a: Explicit memory accounting
     *
     * Make memory usage transparent and verifiable for DoS hardening.
     */
    struct MemoryStats {
        size_t tx_count;                // Number of transactions
        size_t total_bytes;             // Total memory used (tx data + metadata)
        size_t max_bytes;               // Configured maximum
        size_t available_bytes;         // Remaining capacity
        double usage_percent;           // Percentage full
        size_t largest_tx_bytes;        // Largest single transaction
        size_t smallest_tx_bytes;       // Smallest single transaction
        size_t avg_tx_bytes;            // Average transaction size

        // Per-tx breakdown
        size_t tx_data_bytes;           // Actual transaction data
        size_t metadata_bytes;          // MempoolEntry overhead (estimated)
        size_t index_bytes;             // Index overhead (estimated)
    };

    MemoryStats getMemoryStats() const;

    /**
     * Get mempool statistics
     */
    struct Stats {
        size_t count;
        size_t size_bytes;
        uint64_t total_fees;
        double min_fee_rate;
        double max_fee_rate;
        double median_fee_rate;
    };

    Stats getStats() const;

private:
    // Configuration
    MempoolConfig config_;

    // F.9.8: Invalid transaction cache (DoS protection)
    InvalidTxCache invalid_tx_cache_;

    // Transaction storage (Phase M.4.3-C: Type-safe TxId keys)
    mutable std::mutex mutex_;
    std::unordered_map<TxId, std::shared_ptr<MempoolEntry>> entries_;

    // Indexes for efficient queries (Phase M.4.3-C: Type-safe TxId)
    std::set<std::pair<double, TxId>> by_fee_rate_;  // (fee_rate, txid)
    std::set<std::pair<uint64_t, TxId>> by_time_;    // (time, txid)

    // Conflict tracking (outpoint -> txid) (Phase M.4.3-C: Type-safe TxId values, OutPoint.txid already TxId)
    std::unordered_map<OutPoint, TxId> spent_outpoints_;

    // Statistics
    size_t total_size_;
    uint64_t total_fees_;

    // Helper functions
    bool validateTransaction(
        const Transaction& tx,
        const consensus::ChainStateView& coins_view,
        uint32_t current_height,
        uint64_t current_time,
        consensus::TxValidationResult& result
    );

    bool checkConflicts(
        const Transaction& tx,
        std::vector<TxId>& conflicts  // Phase M: TxId identity
    ) const;

    bool checkRBFRules(
        const Transaction& new_tx,
        const std::vector<TxId>& conflicts,  // Phase M: TxId identity
        uint64_t new_fee
    ) const;

    void updateAncestorState(const uint256& txid);  // Phase M.0: Changed to uint256
    void updateDescendantState(const uint256& txid);  // Phase M.0: Changed to uint256

    bool exceedsLimits(const MempoolEntry& entry) const;

    void addToIndexes(const TxId& txid, const MempoolEntry& entry);  // Phase M: TxId identity
    void removeFromIndexes(const TxId& txid);  // Phase M: TxId identity

    size_t calculateVirtualSize(const Transaction& tx) const;
    bool signalsRBF(const Transaction& tx) const;

    // Phase C.2: Covenant detection helper (policy heuristic, not consensus)
    bool detectCovenantScript(const std::vector<uint8_t>& scriptPubKey) const;
};

} // namespace mempool
} // namespace dinero
