#pragma once

/**
 * @file bridge_node.h
 * @brief Phase 7.2: Bridge Node - Utreexo Proof Generator
 *
 * **Purpose:**
 * Bridge nodes maintain both UTXO database and Utreexo forest,
 * enabling them to generate and serve Utreexo proofs to stateless nodes.
 *
 * **Capabilities:**
 * - Generate Utreexo proofs for blocks on-demand
 * - Serve `getutxoproof` requests from stateless nodes
 * - Serve `getutxohdrs` requests with Utreexo commitments
 * - Cache generated proofs for efficiency (LRU)
 *
 * **Security Model:**
 * - Bridge nodes are UNTRUSTED by stateless nodes
 * - All proofs are cryptographically verifiable
 * - Stateless nodes verify: PoW + Root Continuity + Proof Validity
 *
 * **Performance:**
 * - Proof generation: O(n log m) where n = inputs, m = forest size
 * - Typical block (10 inputs): ~1-2 ms
 * - Large block (100 inputs): ~10-20 ms
 * - Cache hit rate: ~80-90% for recent blocks
 */

#include "primitives/uint256.h"
#include "primitives/block.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/interfaces/iutxo_provider.h"
#include "consensus/proof_cache.h"
#include "network/utreexo_messages.h"
#include <memory>
#include <optional>
#include <vector>
#include <chrono>
#include <list>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace dinero {

class ChainDB;  // Forward declaration for height lookups
class BlockStorage;

namespace consensus { class IConsensusUTXOSet; }  // forest lock owner

namespace network {

/**
 * @brief Bridge Node - Generates and serves Utreexo proofs
 *
 * **Usage:**
 * ```cpp
 * BridgeNode bridge(utxo_provider, utreexo_forest, proof_cache);
 *
 * // Generate proof for a block
 * auto proof = bridge.GenerateProofForBlock(block, block_height);
 *
 * // Handle proof request from stateless node
 * auto response = bridge.HandleProofRequest(request);
 * ```
 *
 * **Thread Safety:** Not thread-safe (caller must synchronize)
 */
class BridgeNode {
public:
    struct CacheSnapshot {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        double hit_rate = 0.0;
        size_t block_entries = 0;
        size_t block_capacity = 0;
        size_t tx_entries = 0;
        size_t tx_capacity = 0;
        size_t indexed_heights = 0;
        size_t indexed_blocks = 0;
        uint64_t block_ttl_seconds = 0;
        uint64_t tx_ttl_seconds = 0;

        // Proof-serving data-plane metrics (queue/coalescing/latency).
        uint64_t proof_requests_total = 0;
        uint64_t proof_requests_rejected = 0;
        uint64_t proof_requests_coalesced = 0;
        uint64_t proof_tasks_completed = 0;
        uint64_t proof_tasks_failed = 0;
        uint64_t proof_queue_depth = 0;
        uint64_t proof_queue_capacity = 0;
        uint64_t proof_workers = 0;
        uint64_t active_generations = 0;
        uint64_t tip_priority_accepted = 0;
        uint64_t recent_priority_accepted = 0;
        uint64_t historical_priority_accepted = 0;
        uint64_t tip_priority_rejected = 0;
        uint64_t recent_priority_rejected = 0;
        uint64_t historical_priority_rejected = 0;
        double proof_generation_p50_ms = 0.0;
        double proof_generation_p95_ms = 0.0;
        double proof_generation_p99_ms = 0.0;
        double queue_wait_p50_ms = 0.0;
        double queue_wait_p95_ms = 0.0;
        double queue_wait_p99_ms = 0.0;
    };

    /**
     * @brief Construct bridge node
     *
     * @param utxo_provider UTXO provider (shared ownership for lifetime safety)
     * @param utreexo_forest Utreexo accumulator for generating proofs
     * @param proof_cache Optional LRU cache for proof storage
     * @param owner Forest lock owner. When set, every live-forest read goes
     *        through the owner's shared lock (LockForestShared) so proof
     *        serving on P2P threads cannot race guarded forest writes on the
     *        activation side (TSan finding, issue #578). When null (tests,
     *        standalone forests) reads are direct, as before.
     */
    BridgeNode(
        std::shared_ptr<consensus::IUTXOProvider> utxo_provider,
        consensus::UtreexoForest* utreexo_forest,
        consensus::ProofCache* proof_cache = nullptr,
        ChainDB* chain_db = nullptr,
        BlockStorage* block_storage = nullptr,
        consensus::IConsensusUTXOSet* owner = nullptr
    );

    virtual ~BridgeNode();

    // ═══════════════════════════════════════════════════════════════════════
    // Proof Generation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate Utreexo proof for a block
     *
     * **Algorithm:**
     * 1. Capture accumulator root before block
     * 2. For each spent input:
     *    - Look up UTXO from database
     *    - Compute leaf hash
     *    - Find leaf position in forest
     *    - Generate proof for that position
     * 3. Deduplicate proof hashes (important!)
     * 4. Store spent output metadata
     *
     * **Complexity:** O(n log m) where n = inputs, m = forest size
     *
     * @param block Block to generate proof for
     * @param block_height Height of the block
     * @return BlockUtreexoData with proof and metadata
     * @throws std::runtime_error if UTXO lookup fails or proof generation fails
     */
    consensus::BlockUtreexoData GenerateProofForBlock(
        const Block& block,
        uint32_t block_height
    );

    /**
     * @brief Generate proof for a single UTXO spend
     *
     * **Use Case:** Mempool validation, individual transaction proof
     *
     * @param txid Transaction ID of the UTXO
     * @param vout Output index
     * @return Proof and spent output metadata, or nullopt if UTXO not found
     */
    std::optional<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>>
    GenerateProofForUTXO(const uint256& txid, uint32_t vout);

    /**
     * @brief Generate Utreexo proofs for all inputs of a transaction
     *
     * For each non-coinbase input, calls GenerateProofForUTXO() to produce
     * per-input inclusion proofs + spent output metadata.
     *
     * @param tx Transaction to generate proofs for
     * @return Per-input (UtreexoProof, SpentOutputData) vector, or nullopt if any input fails
     */
    std::optional<std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>>>
    GenerateProofsForTransaction(const Transaction& tx);

    // ═══════════════════════════════════════════════════════════════════════
    // Request Handling
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Result of HandleProofRequest with backpressure info
     *
     * Contains both successful proofs and hashes rejected due to
     * queue backpressure. Callers should send NACK for rejected hashes
     * so peers can implement retry-with-backoff.
     */
    struct ProofRequestResult {
        std::vector<UtreexoProofMessage> proofs;           // Successfully generated proofs
        std::vector<uint256> backpressure_rejected;        // Hashes rejected: queue full
    };

    /**
     * @brief Handle `getutxoproof` request from stateless node
     *
     * **Algorithm:**
     * 1. Validate request (check batch size, block existence)
     * 2. For each requested block:
     *    - Check proof cache first
     *    - If miss, generate proof on-demand
     *    - Store in cache for future requests
     * 3. Return utxoproof messages + any backpressure-rejected hashes
     *
     * **DoS Protection:**
     * - Enforces MAX_BATCH_SIZE (16 blocks)
     * - Rate limiting handled by caller
     *
     * @param request getutxoproof message from peer
     * @param block_provider Callback to look up blocks by hash
     * @return ProofRequestResult with proofs and rejected hashes
     */
    virtual ProofRequestResult HandleProofRequest(
        const GetUtreexoProofMessage& request,
        std::function<std::optional<Block>(const uint256&)> block_provider
    );

    /**
     * @brief Handle `getutxohdrs` request from stateless node
     *
     * **Algorithm:**
     * 1. Parse block locator to find common ancestor
     * 2. Fetch up to 2000 headers forward from that point
     * 3. Include utreexoCommitment in each header
     * 4. Return utxohdrs message
     *
     * **Bitcoin Core Compatibility:** Uses same locator algorithm
     *
     * @param request getutxohdrs message from peer
     * @param header_provider Callback to look up headers
     * @return utxohdrs message with headers + commitments
     */
    virtual UtreexoHeadersMessage HandleHeadersRequest(
        const GetUtreexoHeadersMessage& request,
        std::function<std::optional<BlockHeader>(const uint256&)> header_provider,
        std::function<std::optional<BlockHeader>(uint32_t)> header_by_height_provider
    );

    // ═══════════════════════════════════════════════════════════════════════
    // Caching & Statistics
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Pre-generate and cache proof for a block
     *
     * **Use Case:** Ahead-of-time proof generation for better latency
     *
     * @param block Block to pre-generate proof for
     * @param block_height Height of the block
     * @return true if proof cached successfully
     */
    bool PreCacheProofForBlock(const Block& block, uint32_t block_height);

    /**
     * @brief Get current forest commitment (accumulator root)
     */
    consensus::UtreexoHash GetCurrentForestCommitment() const;

    /**
     * @brief Get proof cache statistics
     *
     * @return Cache stats (hits, misses, hit rate)
     */
    consensus::ProofCache::Stats GetCacheStats() const;

    /**
     * @brief Get detailed block/tx proof cache snapshot
     */
    CacheSnapshot GetCacheSnapshot() const;

    /**
     * @brief Clear proof cache
     */
    void ClearCache();

    /**
     * @brief Invalidate cached transaction proofs (utxotx path)
     *
     * Transaction proofs are tip-root bound. On any tip change, cached tx
     * proofs must be discarded to avoid serving stale-root proofs.
     */
    void InvalidateTxProofCache();

    /**
     * @brief Remove cache entries that are no longer chain-fresh
     *
     * Evicts entries that are expired or no longer match canonical chain
     * metadata (height/hash/root continuity).
     *
     * @return Number of entries evicted
     */
    size_t PruneStaleCacheEntries();

    /**
     * @brief Evict cached block proofs at or above a height
     *
     * Reorg-safe targeted invalidation: keep deep historical cache entries
     * while dropping branch-sensitive entries from the fork boundary upward.
     *
     * @param min_height Inclusive lower bound
     * @return Number of entries evicted
     */
    size_t EvictBlockProofsAtOrAboveHeight(uint32_t min_height);

    /**
     * @brief Set maximum cache size
     *
     * @param max_entries Maximum number of blocks to cache
     */
    void SetCacheSize(size_t max_entries);

    /**
     * @brief Inject post-block metadata into cached proof entry
     *
     * Called by ChainstateService::ConnectTip after ConnectBlock succeeds.
     * Sets the block_height and accumulator_root_after for the cached proof.
     * No-op if block_hash is not in the cache.
     *
     * @param block_hash Block hash identifying the cache entry
     * @param block_height Block height
     * @param root_after Forest commitment after applying this block
     */
    void SetCachedRootAfter(const uint256& block_hash, uint32_t block_height,
                            const consensus::UtreexoHash& root_after);

    /**
     * @brief Cache a transition proof for a block
     * @param block_hash Block hash identifying the cache entry
     * @param tp Transition proof to cache
     */
    void SetCachedTransitionProof(const uint256& block_hash,
                                   const consensus::UtreexoTransitionProof& tp);

    /**
     * @brief Retrieve cached transition proof for a block
     * @param block_hash Block hash
     * @return Transition proof if cached, nullopt otherwise
     */
    std::optional<consensus::UtreexoTransitionProof> GetTransitionProof(
        const uint256& block_hash) const;

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Compute leaf hash for a UTXO
     *
     * **Format:** Hash(txid || vout || value || scriptPubKey)
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @param value Output value in una
     * @param script_pub_key Output script
     * @param created_height Height where the UTXO was created
     * @param is_coinbase Whether the UTXO was created by a coinbase transaction
     * @return 32-byte leaf hash
     */
    consensus::UtreexoHash ComputeLeafHash(
        const uint256& txid,
        uint32_t vout,
        uint64_t value,
        const std::vector<uint8_t>& script_pub_key,
        uint32_t created_height = 0,
        bool is_coinbase = false
    ) const;

    /**
     * @brief Deduplicate proof hashes
     *
     * **Rationale:** Multiple spends may share common proof hashes.
     * Deduplication reduces proof size by ~10-30%.
     *
     * @param proof_hashes Vector of proof hashes (modified in-place)
     */
    void DeduplicateProofHashes(std::vector<consensus::UtreexoHash>& proof_hashes) const;

    /**
     * @brief Find common ancestor from block locator
     *
     * **Bitcoin Core Algorithm:**
     * - Locator: [tip, tip-1, tip-2, tip-3, tip-5, tip-9, tip-17, ...]
     * - Exponential back-off for efficient fork point detection
     *
     * @param locator_hashes Block locator hashes
     * @param header_provider Callback to check if hash is on best chain
     * @return Height of common ancestor, or 0 if genesis
     */
    uint32_t FindCommonAncestor(
        const std::vector<uint256>& locator_hashes,
        std::function<std::optional<uint32_t>(const uint256&)> header_provider
    ) const;

    /**
     * @brief Check whether a block hash is canonical at the given height
     *
     * Uses ChainDB height index and canonical hash mapping to reject
     * side-chain/orphan proof serving during reorg churn.
     *
     * @return true when hash matches best-chain hash at height
     */
    bool IsCanonicalHashAtHeight(const uint256& block_hash, uint32_t height) const;

    /**
     * @brief Why GenerateProofForHashViaEngine returned nullopt
     *
     * Used by HandleProofRequest to distinguish queue-full (send NACK)
     * from other failures (silently skip).
     */
    enum class ProofRejectReason : uint8_t {
        None = 0,        // No rejection (success)
        QueueFull = 1,   // All queues at capacity, no preemption possible
        Stale = 2,       // Proof generated but chain reorged during async work
        WorkerFailed = 3,// Proof worker encountered an error
        Shutdown = 4,    // Node is shutting down
    };

    enum class ProofPriority : uint8_t {
        TipCritical = 0,
        Recent = 1,
        Historical = 2
    };

    struct InflightProofState {
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool success = false;
        std::string error;
        uint32_t block_height = 0;
        consensus::BlockUtreexoData proof_data;
        consensus::UtreexoHash root_after;
        uint32_t waiters = 0;
    };

    struct QueuedProofTask {
        std::string key;
        uint256 block_hash;
        Block block;
        uint32_t block_height = 0;
        ProofPriority priority = ProofPriority::Recent;
        std::shared_ptr<InflightProofState> state;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    // Proof-serving engine helpers.
    std::optional<UtreexoProofMessage> GenerateProofForHashViaEngine(
        const uint256& block_hash,
        const Block& block,
        uint32_t height,
        ProofRejectReason* reject_reason = nullptr
    );
    ProofPriority ClassifyPriority(uint32_t height) const;
    std::string MakeCoalesceKey(const uint256& block_hash, const consensus::UtreexoHash& root_after) const;
    void StartProofWorkers(size_t worker_count);
    void StopProofWorkers();
    void ProofWorkerLoop();
    bool PopNextProofTaskLocked(QueuedProofTask& out_task);
    size_t GetQueuedTaskCountLocked() const;
    void RecordLatencySampleLocked(std::deque<uint64_t>& samples, uint64_t value);
    double ComputePercentileLocked(const std::deque<uint64_t>& samples, double pct) const;
    void FailQueuedTaskLocked(QueuedProofTask&& task, const std::string& error);

    // ═══════════════════════════════════════════════════════════════════════
    // Member Variables
    // ═══════════════════════════════════════════════════════════════════════

    std::shared_ptr<consensus::IUTXOProvider> utxo_provider_;  // UTXO provider (shared ownership)
    consensus::UtreexoForest* utreexo_forest_; // Utreexo accumulator (not owned)
    consensus::IConsensusUTXOSet* owner_;      // Forest lock owner (may be null; not owned)

    // Run fn over the live forest under the owner's SHARED lock (issue #578):
    // excludes guarded exclusive writers while allowing concurrent readers.
    // Ownerless bridges (tests / standalone forests) read directly.
    void readForestShared(
        const std::function<void(const consensus::UtreexoForest&)>& fn) const;
    consensus::ProofCache* proof_cache_;       // Proof cache (optional, not owned)
    ChainDB* chain_db_ = nullptr;              // Block height lookups (optional, not owned)
    BlockStorage* block_storage_ = nullptr;    // Flatfile block body access (optional, not owned)

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 9.3: BlockUtreexoData Cache
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Cached proof entry with TTL
     *
     * **Phase 9.3:** Cache generated proofs to avoid regeneration.
     * Typical use case: same block requested by multiple stateless peers.
     */
    struct CachedProofEntry {
        consensus::BlockUtreexoData proof_data;
        std::optional<consensus::UtreexoTransitionProof> transition_proof;  // Phase 3: Transition proof
        std::chrono::steady_clock::time_point cached_at;
        size_t access_count = 0;
        uint32_t block_height = 0;                     // Block height (set at pre-cache time)
        consensus::UtreexoHash root_after;             // Forest root AFTER applying block (set by SetCachedRootAfter)
    };

    // LRU cache for block proofs (block_hash → proof_data)
    std::unordered_map<uint256, CachedProofEntry> block_proof_cache_;

    // LRU list: front = most recently used, back = least recently used
    std::list<uint256> cache_lru_list_;
    std::unordered_map<uint256, std::list<uint256>::iterator> cache_lru_lookup_;

    // Height index: height -> cached block hashes (used for targeted reorg eviction).
    std::unordered_map<uint32_t, std::unordered_set<uint256>> block_height_index_;

    // Cache configuration
    size_t cache_max_size_ = 1000;  // Phase 9.3: 1000 proofs (~10 MB)
    std::chrono::minutes cache_ttl_{10};  // Phase 9.3: 10 minute TTL

    // Transaction proof cache (utxotx serving): txid -> per-input proofs.
    // Entries are valid only for root_at_generation.
    struct CachedTxProofEntry {
        std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>> proofs;
        consensus::UtreexoHash root_at_generation;
        std::chrono::steady_clock::time_point cached_at;
        size_t access_count = 0;
    };
    std::unordered_map<uint256, CachedTxProofEntry> tx_proof_cache_;
    std::list<uint256> tx_cache_lru_list_;
    std::unordered_map<uint256, std::list<uint256>::iterator> tx_cache_lru_lookup_;
    size_t tx_cache_max_size_ = 500;
    std::chrono::seconds tx_cache_ttl_{30};

    // Cache statistics
    mutable uint64_t cache_hits_ = 0;
    mutable uint64_t cache_misses_ = 0;
    mutable uint64_t cache_evictions_ = 0;

    // Internal cache management helpers (caller must hold cache_mutex_)
    void evictOldestCacheEntry();
    void touchCacheLRU(const uint256& block_hash);
    bool isCacheEntryExpired(const CachedProofEntry& entry) const;
    bool IsCacheEntryChainFresh(const uint256& block_hash, const CachedProofEntry& entry) const;
    void evictOldestTxCacheEntry();
    void touchTxCacheLRU(const uint256& txid);
    bool isTxCacheEntryExpired(const CachedTxProofEntry& entry) const;
    bool eraseBlockCacheEntryLocked(const uint256& block_hash);
    bool eraseTxCacheEntryLocked(const uint256& txid);
    void addBlockHeightIndexLocked(uint32_t height, const uint256& block_hash);
    void removeBlockHeightIndexLocked(uint32_t height, const uint256& block_hash);

    /// Protects block/tx proof caches, LRU lists, and cache statistics
    mutable std::mutex cache_mutex_;

    // Proof-serving data plane: bounded queue + worker pool + request coalescing.
    std::condition_variable proof_queue_cv_;
    std::deque<QueuedProofTask> tip_queue_;
    std::deque<QueuedProofTask> recent_queue_;
    std::deque<QueuedProofTask> historical_queue_;
    std::unordered_map<std::string, std::shared_ptr<InflightProofState>> inflight_proofs_;
    std::vector<std::thread> proof_workers_;
    bool proof_workers_shutdown_ = false;
    size_t proof_worker_count_ = 0;
    size_t proof_queue_capacity_ = 2048;
    size_t active_generations_ = 0;

    // Data-plane metrics.
    uint64_t proof_requests_total_ = 0;
    uint64_t proof_requests_rejected_ = 0;
    uint64_t proof_requests_coalesced_ = 0;
    uint64_t proof_tasks_completed_ = 0;
    uint64_t proof_tasks_failed_ = 0;
    uint64_t tip_priority_accepted_ = 0;
    uint64_t recent_priority_accepted_ = 0;
    uint64_t historical_priority_accepted_ = 0;
    uint64_t tip_priority_rejected_ = 0;
    uint64_t recent_priority_rejected_ = 0;
    uint64_t historical_priority_rejected_ = 0;
    std::deque<uint64_t> proof_generation_latency_ms_;
    std::deque<uint64_t> proof_queue_wait_ms_;
    static constexpr size_t LATENCY_SAMPLE_WINDOW = 2048;
};

} // namespace network
} // namespace dinero
