#pragma once

#include "consensus/outpoint.h"  // Phase M.0: Canonical OutPoint
#include "primitives/uint256.h"  // Phase M.0: uint256 type
#include "daemon/interfaces/ingress_types.h"  // Step 5: Canonical ingress types
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <atomic>
#include <map>
#include <limits>
#include <functional>
#include <optional>
#include "wallet/transaction.h"
#include "common/logger.h"
#include "mempool/coins_view_mempool.h"  // v0.11.0: Policy-layer UTXO overlay
#include "mining/ct_selection_policy.h"  // Phase 3: CT fee policy

// Forward declarations
namespace din::sp {
    class ScannerManager;
}

namespace dinero {

namespace consensus {
class IConsensusUTXOSet;
}

// ============================================================================
// Step 3/5: Structured Transaction Ingress (Phase G.3 → Step 5)
// ============================================================================
//
// TxAcceptResult and TxRejectCode are now defined in:
//   include/daemon/interfaces/ingress_types.h
//
// This header re-exports them for backward compatibility.
// New code should include ingress_types.h directly.
//
// TxAcceptResult is the ONLY return type for transaction ingress.
// All transaction sources (wallet, RPC, P2P, mining) MUST use submitTransaction().
// The legacy addTransaction() is demoted to private adapter.
// ============================================================================

// TxRejectCode and TxAcceptResult are imported via ingress_types.h above

// Forward declarations
class ChainDB;
class ILogger;  // Forward declaration for dependency injection
class FeeEstimator;  // v0.13.0.3: Fee estimation
struct Block;  // Utreexo proof staleness: onBlockConnected/onBlockDisconnected

namespace consensus {
    class ChainStateView;  // Phase M.1: Abstract UTXO query interface
    namespace shielded {
        class CommitmentTree;
        class NullifierSet;
    }
}

// Policy forward declarations
namespace policy {
    class RBFPolicy;
}

/**
 * @brief Transaction entry in the mempool with metadata
 * Phase M.0: Updated to use uint256 and OutPoint
 */
struct MempoolEntry {
    Transaction tx;                                              // The transaction
    uint64_t fee;                                               // Transaction fee in una
    double fee_rate;                                            // Fee per byte (una/byte)
    std::chrono::time_point<std::chrono::steady_clock> time;    // When added to mempool
    uint32_t height;                                            // Block height when added
    size_t tx_size;                                             // Transaction size in bytes
    std::vector<uint256> depends;                               // Dependencies (parent tx hashes) - Phase M.0: uint256
    std::vector<OutPoint> spends;                               // UTXOs this tx spends - Phase M.0: OutPoint

    // STEP 3.6: Package feerate (CPFP support)
    uint64_t ancestor_fee;                                      // Total fee of tx + all ancestors
    size_t ancestor_size;                                       // Total size of tx + all ancestors
    double ancestor_feerate;                                    // ancestor_fee / ancestor_size (CPFP)
    size_t effective_vsize;                                     // CT-aware vsize for this tx
    size_t ancestor_effective_vsize;                            // CT-aware vsize for tx + ancestors
    double ancestor_adjusted_feerate;                           // ancestor_fee / ancestor_effective_vsize

    // Phase 8 Commit 2: Verification Weight Units (VWU) — v7 economic-truth
    // fee metric. For P2MR inputs includes the scheme registry's
    // witness_byte_weight and verify_cost_weight surcharges; for non-P2MR
    // inputs falls back to stripped_size + witness_bytes. See
    // consensus::ComputeVWU in include/consensus/pq/p2mr_consensus.h.
    //
    // Used as the fee-rate denominator for mempool selection, eviction,
    // RBF replacement, and block template sort. NOT a consensus limit —
    // block validity remains on MAX_BLOCK_WEIGHT (BIP141).
    uint64_t vwu = 0;                                           // VWU for this tx alone
    uint64_t ancestor_vwu = 0;                                  // VWU for tx + ancestors

    // CT metadata (for mining template selection)
    bool is_confidential = false;                               // Has confidential outputs
    size_t total_proof_bytes = 0;                               // Total Bulletproof range proof size
    double adjusted_fee_rate = 0.0;                             // Fee rate adjusted for CT verification cost

    // Utreexo proof staleness tracking (CSN mode)
    std::vector<uint8_t> validated_at_root;                     // Accumulator root when TX was validated
    uint32_t validated_at_height = 0;                           // Block height when TX was validated
    bool is_proof_stale = false;                                // True after a block connects post-validation
    std::vector<uint8_t> cached_utxotx_payload;                  // CSN-to-CSN: raw utxotx wire bytes
    uint32_t proof_refresh_attempts = 0;                        // Refresh attempts since last fresh proof

    MempoolEntry() : fee(0), fee_rate(0.0), height(0), tx_size(0),
                     ancestor_fee(0), ancestor_size(0), ancestor_feerate(0.0),
                     effective_vsize(0), ancestor_effective_vsize(0), ancestor_adjusted_feerate(0.0),
                     is_confidential(false), total_proof_bytes(0), adjusted_fee_rate(0.0) {
        time = std::chrono::steady_clock::now();
    }

    MempoolEntry(const Transaction& transaction, uint64_t tx_fee, uint32_t block_height)
        : tx(transaction), fee(tx_fee), height(block_height),
          ancestor_fee(0), ancestor_size(0), ancestor_feerate(0.0),
          effective_vsize(0), ancestor_effective_vsize(0), ancestor_adjusted_feerate(0.0),
          is_confidential(false), total_proof_bytes(0), adjusted_fee_rate(0.0) {
        time = std::chrono::steady_clock::now();
        tx_size = tx.Serialize().size() / 2; // Hex string size / 2 = bytes
        fee_rate = tx_size > 0 ? static_cast<double>(fee) / tx_size : 0.0;
        effective_vsize = std::max<size_t>(tx.GetVirtualSize(), 1);
        ancestor_effective_vsize = effective_vsize;
        ancestor_adjusted_feerate = fee_rate;

        // Compute CT metadata
        for (const auto& output : tx.vout) {
            if (output.is_confidential) {
                is_confidential = true;
                total_proof_bytes += output.range_proof.size();
            }
        }
        adjusted_fee_rate = effective_vsize > 0 ? static_cast<double>(fee) / effective_vsize : fee_rate;
        ancestor_adjusted_feerate = adjusted_fee_rate;
    }
};

/**
 * @brief Memory pool for unconfirmed transactions
 * 
 * The mempool maintains a collection of valid, unconfirmed transactions
 * that are candidates for inclusion in the next block. It handles:
 * - Transaction validation and acceptance
 * - Fee-based prioritization
 * - Dependency tracking
 * - Size management and eviction
 * - Network relay coordination
 */
// Phase M.0: daemon::Mempool (high-level mempool, separate from TxMempool)
class Mempool {
public:
    struct RBFRuntimeConfig {
        bool enabled = false;
        uint64_t min_relay_fee_rate = 0;
        uint64_t incremental_relay_fee = 0;
        size_t max_replacement_count = 0;
    };

    explicit Mempool(ChainDB* chain_db,
                     const consensus::IConsensusUTXOSet* consensus_utxo_set = nullptr);
    ~Mempool();

    // ========================================================================
    // CANONICAL TRANSACTION INGRESS (Step 3 - Phase G.3)
    // ========================================================================
    //
    // submitTransaction() is the SOLE public entry point for ALL transaction
    // acceptance. All sources (wallet, RPC, P2P, mining) MUST use this API.
    //
    // Returns structured TxAcceptResult with:
    // - Machine-readable rejection code
    // - Human-readable error message
    // - Transaction ID for tracking
    //
    // The legacy addTransaction() is PRIVATE and exists only as adapter.
    // ========================================================================

    /**
     * Submit transaction to mempool (CANONICAL INGRESS)
     *
     * This is the ONLY public method for transaction acceptance.
     * All transaction sources MUST use this method.
     *
     * @param tx      Transaction to submit
     * @param source  Origin identifier (for logging/debugging)
     * @param relay   Whether to broadcast to network (default: true)
     * @return        Structured result with rejection code and message
     */
    TxAcceptResult submitTransaction(const Transaction& tx, const std::string& source, bool relay = true);

    // Core mempool operations (Phase M.0: Changed to uint256)
    bool removeTransaction(const uint256& txid);
    bool hasTransaction(const uint256& txid) const;
    std::shared_ptr<Transaction> getTransaction(const uint256& txid) const;

    // STEP 2: TEST_ONLY transaction submission (bypasses signature validation)
    // Note: Still returns structured result for consistency
    TxAcceptResult submitTransactionTestOnly(const Transaction& tx, const std::string& source);

    // Unchecked insertion: no validation, no fee calc, no UTXO lookups.
    // Transparent-only test helper for synthetic mempool state.
    // Private / confidential / covenant transactions must still go through
    // canonical ingress so proof verification cannot be bypassed.
    void addUnchecked(const Transaction& tx);

    // STEP 3: Check if output is spent in mempool (for wallet coin selection)
    // Phase M.0: Now uses OutPoint directly
    bool isOutputSpentInMempool(const OutPoint& outpoint) const;

    // Entry accessors (for RPC fee information) (Phase M.0: Changed to uint256)
    std::optional<MempoolEntry> getMempoolEntry(const uint256& txid) const;
    std::optional<uint64_t> getTransactionFee(const uint256& txid) const;
    std::optional<double> getTransactionFeeRate(const uint256& txid) const;

    /**
     * Read-only iteration over all mempool entries
     *
     * Thread-safe, const-correct, non-leaky iteration primitive.
     * Enables analyzers, histograms, diagnostics without exposing internals.
     *
     * @param fn Callback invoked for each entry (must not modify mempool)
     */
    void forEachEntry(const std::function<void(const MempoolEntry&)>& fn) const;

    // Mempool queries (Phase M.0: Changed to uint256)
    std::vector<Transaction> getAllTransactions() const;
    std::vector<Transaction> getTransactionsByFeeRate(size_t max_count = 1000) const;
    std::vector<uint256> getTransactionIds() const;
    std::vector<Transaction> getTransactionsForAddress(const std::string& address) const;
    size_t size() const;
    uint64_t getTotalFees() const;
    size_t getTotalSize() const;
    
    // Block template creation.
    //
    // next_block_height: height at which the produced template will land.
    // When non-zero, height-gated consensus checks that can change between
    // mempool admission and mining selection are re-applied for that target
    // height. This prevents stale mempool transactions from being assembled
    // into an invalid activation-boundary block.
    std::vector<Transaction> selectTransactionsForBlock(
        size_t max_block_size = 1000000,    // 1MB default
        uint64_t max_block_weight = 4000000, // 4M weight units
        uint32_t next_block_height = 0
    ) const;

    /**
     * Temporarily exclude a mempool transaction from block template assembly.
     *
     * Used as a mining safety valve when a relayed transaction passes mempool
     * admission but later causes deterministic template assembly or
     * self-verification failure. The transaction remains in the mempool for
     * diagnostics and relay, but miners skip it until the exclusion expires.
     */
    void excludeFromBlockTemplates(const uint256& txid,
                                   const std::string& reason,
                                   std::chrono::seconds duration = std::chrono::minutes(30));

    /**
     * Check whether a transaction is currently excluded from block templates.
     */
    bool isExcludedFromBlockTemplates(const uint256& txid, std::string* reason = nullptr) const;
    
    // Mempool management (Phase M.0: Changed to uint256)
    void removeConfirmedTransactions(const std::vector<uint256>& confirmed_txids);
    void removeExpiredTransactions();
    void limitMempoolSize();
    void clear();

    // ========================================================================
    // Utreexo Proof Staleness (CSN Mode)
    // ========================================================================

    /**
     * Process a connected block: evict double-spends, mark remaining TXs stale.
     * Called by ChainstateService::notifyBlockConnected().
     * @return Number of transactions evicted due to input conflicts
     */
    size_t onBlockConnected(const Block& block, uint32_t height,
                            const std::vector<uint8_t>& new_root = {});

    /**
     * Process a disconnected block: mark all TXs stale (root changed).
     */
    void onBlockDisconnected(const Block& block, uint32_t height);

    /** Get count of transactions with stale proofs. */
    size_t getStaleCount() const;

    /** Get txids of all transactions with stale proofs. */
    std::vector<uint256> getStaleTxIds() const;

    /**
     * Apply stale-proof policy and return txids eligible for ONE refresh round.
     *
     * Policy:
     * - If stale set is too large, bulk-evict stale entries (overload protection)
     * - Evict stale entries older than max_proof_age_blocks
     * - Evict stale entries that exceeded max_refresh_attempts
     * - Increment per-tx refresh attempt counter for returned candidates
     */
    std::vector<uint256> selectStaleForRefresh(
        uint32_t chain_height,
        size_t max_refresh_batch = 20,
        uint32_t max_proof_age_blocks = 2,
        uint32_t max_refresh_attempts = 1,
        size_t stale_overload_threshold = 256
    );

    /**
     * Mark a transaction's proof as fresh (after re-validation).
     * @return true if TX exists and was refreshed
     */
    bool refreshProof(const uint256& txid, const std::vector<uint8_t>& new_root,
                      uint32_t new_height);

    /** Store raw utxotx wire payload for CSN-to-CSN relay. */
    bool setCachedUtxoTxPayload(const uint256& txid, std::vector<uint8_t> payload);

    /** Retrieve cached utxotx payload. Returns nullopt if not found, stale, or empty. */
    std::optional<std::vector<uint8_t>> getCachedUtxoTxPayload(const uint256& txid) const;

    // ========================================================================
    // Reorg Handling
    // ========================================================================

    /**
     * Reconcile mempool after a blockchain reorganization
     *
     * When a reorg occurs:
     * 1. Transactions from disconnected blocks become unconfirmed
     * 2. These should be returned to mempool (if still valid)
     * 3. Transactions in new connected blocks are already removed
     *
     * This method:
     * - Takes transactions from disconnected blocks
     * - Filters out any that are now in connected blocks (by txid)
     * - Re-validates remaining against the current UTXO set
     * - Re-adds valid transactions to mempool
     *
     * @param disconnected_txs   Transactions from disconnected blocks
     * @param connected_txs      Transactions from newly connected blocks (to filter)
     * @return                   Number of transactions successfully restored
     */
    size_t ReconcileAfterReorg(
        const std::vector<Transaction>& disconnected_txs,
        const std::vector<Transaction>& connected_txs
    );

    // Statistics and monitoring
    struct MempoolStats {
        size_t tx_count;
        size_t total_size;
        uint64_t total_fees;
        double avg_fee_rate;
        double median_fee_rate;
        size_t min_fee_rate;
        size_t max_fee_rate;
        std::chrono::seconds oldest_tx_age;
        size_t stale_tx_count = 0;          // TXs with stale Utreexo proofs
        uint32_t last_connected_height = 0; // Height of most recent connected block
        uint64_t stale_evicted_total = 0;   // Stale-proof TXs evicted by policy
        uint64_t refresh_attempted_total = 0; // Proof refresh attempts scheduled
        uint64_t refresh_succeeded_total = 0; // Proof refreshes that became fresh again
        uint64_t refresh_dropped_budget_total = 0; // Refreshes dropped due to overload guard
    };
    MempoolStats getStats() const;

    /**
     * Compute VWU (Verification Weight Units) for a transaction. Phase 8
     * Commit 2 fee metric — the economic-truth denominator used for
     * selection, RBF, eviction, and the block-template intelligent
     * selector. Resolves prevouts via the mempool's UTXO view so P2MR
     * inputs pick up the scheme registry surcharges; falls back to
     * stripped + witness_bytes on any lookup failure (defensive — malformed
     * txs can't pass validation anyway, but the denominator must always
     * be defined). Public because BlockAssembler calls it directly.
     */
    uint64_t computeVWUForTx(const Transaction& tx) const;

    // Configuration
    void setMaxSize(size_t max_size) { m_max_size = max_size; }
    void setMaxAge(std::chrono::hours max_age) { m_max_age = max_age; }
    void setMinFeeRate(double min_fee_rate) { m_min_fee_rate = min_fee_rate; }
    double getMinFeeRate() const { return m_min_fee_rate; }

    /**
     * Enable/disable RBF (Replace-By-Fee)
     * Default: false (off) - preserves payment finality for merchants
     * Enable via config: mempool.enable_rbf=true
     */
    void setRBFEnabled(bool enabled);

    /**
     * Check if RBF is enabled
     */
    bool isRBFEnabled() const;

    /**
     * Return the actual runtime RBF policy the mempool is enforcing.
     */
    RBFRuntimeConfig getRBFRuntimeConfig() const;

    // CT Fee Policy Configuration (Phase 3)
    void SetCTConfig(const mining::CTSelectionConfig& config) { ct_config_ = config; }
    const mining::CTSelectionConfig& GetCTConfig() const { return ct_config_; }
    mining::CTSelectionConfig& GetCTConfig() { return ct_config_; }

    // Network integration (Phase M.0: Changed to uint256)
    void broadcastTransaction(const uint256& txid);

    // Transaction broadcast callback (active relay path)
    // Used by MempoolService to wire P2PService for tx relay
    // Only txid is passed - the callback can fetch tx from mempool if needed
    using TxBroadcastCallback = std::function<void(const uint256& txid)>;
    void setTxBroadcastCallback(TxBroadcastCallback callback) { m_tx_broadcast_callback = callback; }

    // Transaction accepted callback (wallet notifier path)
    // Called after a tx is accepted into the mempool, with the full Transaction object.
    // Used by NodeCore to notify watched-script wallets of mempool events.
    using TxAcceptedCallback = std::function<void(const Transaction& tx)>;
    void setTxAcceptedCallback(TxAcceptedCallback callback) { m_tx_accepted_callback = callback; }

    // Logger dependency injection
    void setLogger(ILogger* logger) { m_logger = logger; }

    // UTXO view access (for validation and mining)
    CoinsViewMemPool& getCoinsView() { return coins_view_; }
    const CoinsViewMemPool& getCoinsView() const { return coins_view_; }

    // Mempool persistence (v0.13.0.2 - STEP A/B/C)
    // Rules:
    // - saveToDisk(): Best effort only, never throws, never blocks shutdown forever
    // - loadFromDisk(): Revalidates all transactions against current policy
    // - Persistence does NOT bypass policy - mempool remains gatekeeper
    bool saveToDisk(const std::string& filepath);
    bool loadFromDisk(const std::string& filepath);
    std::string getDefaultMempoolPath() const;

    // Fee estimation (v0.13.0.3 - Bitcoin Core conservative approach)
    // Access to fee estimator for RPC queries
    class FeeEstimator& getFeeEstimator() { return *fee_estimator_; }
    const class FeeEstimator& getFeeEstimator() const { return *fee_estimator_; }

    void setShieldedState(const consensus::shielded::CommitmentTree* tree,
                          const consensus::shielded::NullifierSet* nullifiers) {
        shielded_tree_ = tree;
        shielded_nullifiers_ = nullifiers;
    }

private:
    // ========================================================================
    // LEGACY ADAPTER (Step 3 - Phase G.3)
    // ========================================================================
    // addTransaction() is PRIVATE. It exists only to support internal code
    // that hasn't been migrated yet. New code MUST use submitTransaction().
    //
    // This method delegates to submitTransactionInternal() and discards the
    // structured result, returning only bool for backwards compatibility.
    // ========================================================================
    bool addTransaction(const Transaction& tx, bool relay = true);

    // Internal implementation that returns structured result
    TxAcceptResult submitTransactionInternal(const Transaction& tx, const std::string& source, bool relay);

    // Internal validation and management (Phase M.0: Changed to uint256)
    bool validateTransaction(
        const Transaction& tx,
        std::string& error,
        std::optional<uint32_t> target_height = std::nullopt) const;
    bool checkDoubleSpend(const Transaction& tx) const;
    bool checkDependencies(const Transaction& tx) const;
    uint64_t calculateFee(const Transaction& tx) const;
    std::optional<consensus::UTXOEntry> recoverConflictedInputUTXO(const OutPoint& outpoint) const;
    void updateDependencies(const uint256& txid);
    void recalcAncestorMetrics(MempoolEntry& entry);
    void evictTransactions();
    void evictTransactionsLocked();
    bool removeTransactionLocked(const uint256& txid);  // Lock-free version (caller holds m_mutex)
    void rebuildCoinsViewLocked();
    uint64_t getTotalFeesLocked() const;
    size_t getTotalSizeLocked() const;
    bool isSelectableAtHeightLocked(const MempoolEntry& entry,
                                    uint32_t next_block_height,
                                    std::string* reason = nullptr) const;
    bool isTemplateExcludedLocked(const uint256& txid,
                                  const std::chrono::steady_clock::time_point& now,
                                  std::string* reason = nullptr) const;

    // Data structures (Phase M.0: Changed to uint256 and OutPoint)
    std::unordered_map<uint256, MempoolEntry> m_transactions;      // txid -> entry
    std::unordered_set<OutPoint> m_spent_outputs;                  // OutPoint tracking (replaced string concat)
    std::multimap<double, uint256> m_fee_index;                    // package selection score -> txid (sorted)
    std::multimap<std::chrono::time_point<std::chrono::steady_clock>, uint256> m_time_index; // time -> txid
    std::unordered_map<uint256, std::unordered_set<uint256>> m_children_index; // parent → children
    struct TemplateExclusion {
        std::chrono::steady_clock::time_point expires_at;
        std::string reason;
    };
    std::unordered_map<uint256, TemplateExclusion> m_template_exclusions;
    
    // Utreexo staleness tracking
    std::vector<uint8_t> current_accumulator_root_;  // Updated on each block connect
    uint32_t current_block_height_ = 0;              // Updated on each block connect

    // Thread safety
    mutable std::shared_mutex m_mutex;
    
    // Configuration
    size_t m_max_size;                    // Maximum mempool size (bytes)
    std::chrono::hours m_max_age;         // Maximum transaction age
    double m_min_fee_rate;                // Minimum fee rate (una/byte)
    mining::CTSelectionConfig ct_config_; // CT fee policy configuration (Phase 3)
    
    // Dependencies
    ChainDB* chain_db_;  // Non-owning pointer - single source of truth for chain state
    const consensus::shielded::CommitmentTree* shielded_tree_ = nullptr;
    const consensus::shielded::NullifierSet* shielded_nullifiers_ = nullptr;
    std::unique_ptr<consensus::ChainStateView> chain_state_view_;  // Adapter: ChainDB -> ChainStateView interface (Phase M.1)
    CoinsViewMemPool coins_view_;  // v0.11.0: In-memory UTXO overlay for policy validation
    TxBroadcastCallback m_tx_broadcast_callback;
    TxAcceptedCallback m_tx_accepted_callback;
    ILogger* m_logger;  // Logger dependency injection

    // Helper macros for cleaner DI logging
    #define MPLOG_INFO(msg)  if (m_logger) m_logger->info(msg)
    #define MPLOG_DEBUG(msg) if (m_logger) m_logger->debug(msg)
    #define MPLOG_WARN(msg)  if (m_logger) m_logger->warning(msg)
    #define MPLOG_ERR(msg)   if (m_logger) m_logger->error(msg)

    // Silent Payments scanner
    std::unique_ptr<din::sp::ScannerManager> m_sp_scanner_manager;

    // BIP125 RBF policy enforcement
    std::unique_ptr<policy::RBFPolicy> m_rbf_policy;

    // Fee estimation (v0.13.0.3)
    std::unique_ptr<FeeEstimator> fee_estimator_;

    // Statistics
    std::atomic<size_t> m_total_tx_added{0};
    std::atomic<size_t> m_total_tx_removed{0};
    std::atomic<size_t> m_total_tx_rejected{0};
    std::atomic<uint64_t> m_stale_evicted_total{0};
    std::atomic<uint64_t> m_refresh_attempted_total{0};
    std::atomic<uint64_t> m_refresh_succeeded_total{0};
    std::atomic<uint64_t> m_refresh_dropped_budget_total{0};
    
    // Constants
    static constexpr size_t DEFAULT_MAX_SIZE = 300 * 1024 * 1024;  // 300MB
    static constexpr std::chrono::hours DEFAULT_MAX_AGE{336};       // 336 hours = 2 weeks (Bitcoin Core default)
    static constexpr double DEFAULT_MIN_FEE_RATE = 1.0;            // 1 sat/byte
};

} // namespace dinero
