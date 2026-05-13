#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <atomic>
#include <optional>
#include <functional>
#include <unordered_set>  // v0.14.0.1: For ensureAncestorsIncluded
#include <unordered_map>  // Phase 8.5 Commit 2: per-scheme input caps

// Include the actual definitions
#include "consensus/subsidy.h"
#include "consensus/difficulty.h"
#include "consensus/utreexo_accumulator.h"  // Utreexo proof generation
#include "consensus/interfaces/iutxo_provider.h"  // UTXO lookup for spent outputs
#include "primitives/block.h"
#include "wallet/transaction.h"
#include "mining/ct_selection_policy.h"  // CT-specific selection policies

// Forward declarations
namespace dinero {
    class ChainDB;  // ChainDB for context injection
    // Phase 39: ChainManager forward declaration removed (ChainManager deleted)
    struct ChainParams;
    struct SupplyState;
    class BlockRelayManager;  // Phase W.1.3: For network context

    namespace consensus {
        class UtreexoForest;  // Utreexo accumulator
        class IUTXOProvider;  // UTXO provider interface
        class BlockValidator; // Utreexo root computation oracle
    }
}

namespace dinero {

std::optional<OutPoint> ParseTemplatePoisonMissingPrevout(const std::string& error);
std::unordered_set<uint256> CollectTemplatePoisonRemovalSet(
    const std::vector<Transaction>& candidate_txs,
    const OutPoint& missing_prevout,
    std::unordered_set<uint256>* direct_spenders = nullptr
);
std::vector<Transaction> FilterChainBackedTemplateTransactions(
    const std::vector<Transaction>& candidate_txs,
    const std::function<bool(const OutPoint&)>& has_chain_utxo,
    std::unordered_set<uint256>* deferred_txids = nullptr
);

/**
 * @brief Mining job containing all data needed for block mining
 * 
 * This structure contains a complete block template ready for mining,
 * including the header, transactions, and mining metadata.
 */
struct MiningJob {
    // Block data
    BlockHeader header;
    std::vector<Transaction> transactions;  // coinbase first

    // Mining parameters
    uint32_t target_bits{0};
    uint32_t current_time{0};
    uint64_t extra_nonce{0};

    // Optimization data
    std::vector<uint8_t> midstate;  // SHA256 midstate for optimization
    std::string merkle_root;
    std::string utreexo_root;       // 32-byte hex - AFTER-state Utreexo commitment (Dinero 112-byte header)
    
    // Job metadata
    std::string job_id;
    uint32_t height{0};
    uint64_t block_reward{0};
    uint64_t total_fees{0};
    
    // Mining target (for validation)
    std::string target_hex;
    
    // Timing
    uint32_t created_time{0};
    uint32_t max_time{0};  // Maximum valid timestamp
    
    // State tracking
    bool is_stale{false};
    std::atomic<bool> stop_mining{false};
    std::atomic<bool> solution_claimed{false};  // Winner-takes-all: only first solution submits

    // Constructor: Enforce invariant that every new job starts with stop_mining=false
    MiningJob()
        : header{}
        , stop_mining(false)
        , solution_claimed(false)
    {
        // Header hash commits to reserved[12], so every new job must start from
        // a fully zeroed BlockHeader before individual fields are assigned.
        header.ZeroReserved();
    }
};

/**
 * @brief Block assembler for creating mining jobs
 * 
 * This class is responsible for assembling blocks from the mempool,
 * calculating appropriate rewards and fees, and creating mining jobs
 * that can be distributed to miners.
 */
class BlockAssembler {
public:
    // Phase 39: ChainManager parameter removed (ChainManager deleted)
    explicit BlockAssembler(ChainDB* chain_db);
    ~BlockAssembler() = default;
    
    // Set mining address for script generation
    void SetMiningAddress(const std::string& address);

    /**
     * @brief Set BlockRelayManager for network-aware mining (Phase W.1.3)
     *
     * Enables intelligent transaction selection based on compact block success,
     * sync phase, and mempool pressure.
     *
     * @param relay_manager BlockRelayManager instance (non-owning pointer)
     */
    void SetBlockRelayManager(BlockRelayManager* relay_manager) {
        block_relay_manager_ = relay_manager;
    }

    /**
     * @brief Enable/disable intelligent transaction selection (Phase W.1.3)
     *
     * When enabled, uses TransactionScorer for context-aware transaction ranking.
     * When disabled, falls back to standard CPFP-aware fee sorting.
     *
     * @param enable True to enable intelligent selection
     */
    void SetIntelligentSelection(bool enable) {
        use_intelligent_selection_ = enable;
    }

    /**
     * @brief Set CT selection policy for confidential transaction handling
     *
     * Enables CT-specific policies for block template creation:
     * - Weight multiplier for proof verification cost
     * - Per-block CT limits to prevent DoS
     * - Minimum fee rate for CT transactions
     * - Batch verification optimization hints
     *
     * @param policy CT selection policy (takes ownership)
     */
    void SetCTSelectionPolicy(std::unique_ptr<mining::CTSelectionPolicy> policy) {
        ct_policy_ = std::move(policy);
    }

    /**
     * @brief Enable/disable CT transactions in block templates
     *
     * When disabled, CT transactions are excluded from templates.
     * This is separate from the kill-switch (which rejects CT at consensus).
     *
     * @param enabled True to include CT transactions
     */
    void SetCTEnabled(bool enabled) {
        ct_enabled_ = enabled;
    }

    /**
     * @brief Check if CT transactions are enabled for mining
     */
    bool IsCTEnabled() const {
        return ct_enabled_;
    }

    /**
     * @brief Set Utreexo forest for proof generation (Utreexo integration)
     *
     * Enables miner to generate batched Utreexo proofs for block templates.
     * Required for Utreexo-enforced blocks.
     *
     * @param forest Utreexo forest instance (non-owning pointer)
     */
    void SetUtreexoForest(consensus::UtreexoForest* forest) {
        utreexo_forest_ = forest;
    }

    /**
     * @brief Set UTXO provider for spent output data collection (Utreexo integration)
     *
     * Enables miner to collect spent output metadata (value, scriptPubKey,
     * confidential flag, commitment)
     * required for stateless Utreexo validation.
     *
     * @param provider UTXO provider (shared ownership for lifetime safety)
     */
    void SetUTXOProvider(std::shared_ptr<consensus::IUTXOProvider> provider) {
        utxo_provider_ = std::move(provider);
    }

    /**
     * @brief Set BlockValidator for Utreexo root computation (single source of truth)
     *
     * Enables BlockAssembler to call the same ComputeUtreexoRootPure function
     * used by validation, ensuring mining and validation compute identical roots.
     *
     * @param validator BlockValidator instance (non-owning pointer)
     */
    void SetBlockValidator(consensus::BlockValidator* validator) {
        block_validator_ = validator;
    }

    /**
     * @brief Create a new mining job
     * 
     * Assembles a new block template with transactions from the mempool,
     * calculates the appropriate coinbase reward, and prepares it for mining.
     * Uses the configured mining payout address from mining_payout_resolver.
     * 
     * @return MiningJob Complete mining job ready for hashing
     */
    std::shared_ptr<MiningJob> CreateJob(const uint256* explicit_tip_hash = nullptr);

    /**
     * @brief Refresh an existing mining job
     * 
     * Updates the timestamp, extra nonce, and potentially the transaction
     * set if the mempool has changed significantly.
     * 
     * @param job Mining job to refresh
     * @return bool True if job was successfully refreshed
     */
    bool RefreshJob(std::shared_ptr<MiningJob> job);

    /**
     * @brief Update job with new timestamp and extra nonce
     * 
     * Called when the nonce space is exhausted or periodically
     * to keep the block timestamp current.
     * 
     * @param job Mining job to update
     * @return bool True if update was successful
     */
    bool UpdateJobTime(std::shared_ptr<MiningJob> job);

    /**
     * @brief Check if job needs refresh
     * 
     * Determines if a mining job should be refreshed based on:
     * - Age of the job
     * - Changes in mempool
     * - New blocks received
     * 
     * @param job Mining job to check
     * @return bool True if job should be refreshed
     */
    bool ShouldRefreshJob(const std::shared_ptr<MiningJob>& job) const;

    /**
     * @brief Get current mining statistics
     *
     * @return std::string Formatted mining statistics
     */
    std::string GetMiningStats() const;

    // ========================================================================
    // v0.14.0.1: Bitcoin Core Compatible RPC Mining Interface
    // ========================================================================

    /**
     * @brief Create a new block template (Bitcoin Core compatible)
     *
     * This is the Bitcoin Core-compatible interface for RPC mining
     * (getblocktemplate/submitblock). It returns a complete Block ready
     * for mining, with deterministic transaction selection.
     *
     * v0.14.0.1 (FOUNDATION) - Deterministic block template assembly
     *
     * Responsibilities:
     * - Select transactions from mempool (fee-optimal, CPFP-aware)
     * - Respect block weight limit (4M weight units)
     * - Respect ancestor/descendant rules
     * - Apply package feerate (CPFP)
     * - Deterministic ordering (tie-breaks)
     *
     * Design Principles:
     * - Same mempool state → same block template (deterministic)
     * - Fee-optimal selection (maximize revenue, not tx count)
     * - Never violate consensus rules
     * - Bitcoin Core compatible behavior
     *
     * @param coinbase_address Address to receive coinbase reward + fees
     * @return Complete block template ready for mining, or nullptr on failure
     */
    std::unique_ptr<Block> CreateNewBlock(const std::string& coinbase_address);

    /**
     * @brief Get statistics from the last CreateNewBlock() call
     */
    struct BlockTemplateStats {
        size_t total_txs;              // Total transactions in template
        uint64_t total_fees;           // Total fees collected
        uint64_t block_weight;         // Total block weight
        size_t block_size;             // Total block size (bytes)
        size_t mempool_size;           // Mempool size when template created
        size_t rejected_txs;           // Transactions rejected due to limits
        uint32_t height;               // Block height
        std::string prev_block;        // Previous block hash
        uint256 determinism_hash;      // Template determinism guard (txid order + flags)

        // ================================================================
        // WITNESS-BASED EXTRANONCE (Utreexo-compatible)
        // ================================================================
        // Byte offset in serialized coinbase where witness nonce lives.
        // Miners inject extranonce at this offset WITHOUT changing txid.
        size_t witness_nonce_offset = 0;
        size_t witness_nonce_size = 0;   // Should be 8 (bytes)
    };

    /**
     * @brief Transaction entry flags for determinism hash
     *
     * These flags capture everything that affects validation semantics.
     * Used to compute template determinism hash to detect silent mutations.
     */
    struct TxEntryFlags {
        bool is_coinbase = false;      // Coinbase transaction
        bool is_confidential = false;  // Has CT outputs
        uint8_t witness_version = 0;   // SegWit version (0, 1=taproot, etc.)
        uint8_t fee_class = 0;         // Fee rate tier (0=dust, 1=low, 2=med, 3=high, 4=urgent)

        // Compute flags byte for hashing
        uint32_t ToFlagsByte() const {
            return (is_coinbase ? 0x01 : 0) |
                   (is_confidential ? 0x02 : 0) |
                   ((witness_version & 0x0F) << 4) |
                   ((fee_class & 0x0F) << 8);
        }
    };

    /**
     * @brief Compute determinism hash over template transactions
     *
     * Hashes txid || flags for each transaction to create a fingerprint
     * that detects any silent reordering or flag mutations.
     *
     * @param transactions Transaction list (coinbase first)
     * @param flags Corresponding flags for each transaction
     * @return SHA256 hash of the template structure
     */
    static uint256 ComputeTemplateDeterminismHash(
        const std::vector<Transaction>& transactions,
        const std::vector<TxEntryFlags>& flags
    );
    BlockTemplateStats getBlockTemplateStats() const { return block_template_stats_; }
    const std::string& getLastTemplateError() const { return last_template_error_; }

    // Configuration
    void SetMaxBlockWeight(uint32_t max_weight) { max_block_weight_ = max_weight; }
    void SetMinTxFee(uint64_t min_fee) { min_tx_fee_ = min_fee; }
    void SetMaxBlockTime(uint32_t max_time) { max_block_time_ = max_time; }

    /**
     * Phase 8.5: Miner-policy cap on total VWU per block template.
     *
     * NOT a consensus rule — blocks that exceed this from other miners
     * stay consensus-valid. This exists so a locally-built template
     * can't saturate the miner's own verify budget with a signature-
     * heavy workload. Default calibrated for ~1s worst-case verify on
     * a ~190-P2MR-input block. Set to 0 to disable.
     *
     * When VWU eventually promotes to a consensus cap (Phase 9), the
     * miner-policy default becomes the floor of the consensus rule
     * so nodes migrate cleanly.
     */
    void SetMaxBlockVWU(uint64_t max_vwu) { max_block_vwu_ = max_vwu; }
    uint64_t GetMaxBlockVWU() const { return max_block_vwu_; }

    /**
     * Phase 8.5 Commit 2: Miner-policy cap on per-scheme spend-input
     * count per block template.
     *
     * Belt-and-suspenders layer on top of MAX_BLOCK_VWU. If VWU pricing
     * mis-judges a scheme's verify cost (calibration drift, or a
     * pathological input size that happens to match a breakpoint), a
     * count-based ceiling still bounds the worst-case verify time per
     * block. Applies per PQ scheme_id — FALCON-512 (when it activates)
     * would get its own cap, independent of ML-DSA-65's.
     *
     * Default for ML-DSA-65: 200 inputs per block (~1.4 s worst-case
     * verify on commodity hardware, conservative relative to the 190
     * from the VWU cap so one side dominates on typical workloads and
     * the other is a safety net).
     *
     * Non-P2MR inputs are unaffected — this is purely for PQ schemes.
     * Set a scheme's cap to 0 to disable for that scheme.
     */
    void SetMaxP2MRInputsPerBlock(uint8_t scheme_id, uint32_t max_inputs);
    uint32_t GetMaxP2MRInputsPerBlock(uint8_t scheme_id) const;

    // Week 5: ChainDB for context injection
    void setChainDB(class ChainDB* chain_db);
    class ChainDB* getChainDB() const { return chain_db_; }

    // Week 7: Mempool for transaction selection
    void setMempool(class Mempool* mempool);
    class Mempool* getMempool() const { return mempool_; }

    // ========================================================================
    // Phase M.0: Merkle tree calculation (public for golden testing)
    // ========================================================================
    /**
     * @brief Calculate merkle root from transaction list
     *
     * Phase M.0 compliant: Works with uint256 binary identity,
     * converts to hex only at final output boundary.
     *
     * Made public to enable comprehensive golden vector testing
     * for preventing merkle tree calculation regressions.
     *
     * Static to ensure it remains a pure function with no assembler state dependency.
     * This is the ONE canonical merkle algorithm used across mining, validation, and RPC.
     *
     * @param transactions Vector of transactions
     * @return Merkle root as 64-character hex string
     */
    static std::string CalculateMerkleRoot(const std::vector<Transaction>& transactions);

private:
    // Core functionality (Stratum mining)
    std::vector<Transaction> SelectTransactions(uint32_t max_weight, uint64_t& total_fees);
    std::string BuildCoinbaseTransaction(uint32_t height, uint64_t reward, uint64_t fees,
                                       const std::vector<uint8_t>& payout_script);
    std::string GenerateJobId();

    // Validation helpers
    bool ValidateJobParameters(const std::shared_ptr<MiningJob>& job) const;
    uint32_t CalculateBlockWeight(const std::vector<Transaction>& transactions) const;
    uint32_t GetMedianTimePast() const;
    std::string BitsToTargetHex(uint32_t bits);

    // State management
    void UpdateSupplyState();
    void UpdateAlgoState();

    // Mining script generation
    std::vector<uint8_t> GetMiningScript();

    // ========================================================================
    // v0.14.0.1: Bitcoin Core RPC Mining - Private Helpers
    // ========================================================================

    // Transaction selection (fee-optimal, CPFP-aware, deterministic)
    struct MempoolEntry;  // Forward declare to avoid circular dependency
    std::vector<Transaction> selectTransactionsForBlock(
        uint32_t max_weight,
        uint64_t& total_fees_out,
        std::vector<std::string>& included_txids_out
    );

    /**
     * @brief Intelligent transaction selection (Phase W.1.3)
     *
     * Uses TransactionScorer for context-aware transaction ranking based on:
     * - Fee rate (primary factor)
     * - Mining probability (age + fee percentile)
     * - Compact reconstructability (propagation + adaptive bias)
     *
     * @param max_weight Maximum block weight
     * @param total_fees_out Output: total fees collected
     * @param included_txids_out Output: list of included transaction IDs
     * @return Vector of selected transactions
     */
    std::vector<Transaction> selectTransactionsIntelligent(
        uint32_t max_weight,
        uint64_t& total_fees_out,
        std::vector<std::string>& included_txids_out
    );

    // CPFP (Child Pays For Parent) support (v0.14.0.2)
    double calculateAncestorFeerate(const std::string& txid) const;
    std::vector<std::string> getUnconfirmedAncestors(const std::string& txid) const;

    // Block construction helpers
    Transaction createCoinbaseTransaction(
        uint32_t height,
        const std::string& coinbase_address,
        uint64_t block_subsidy,
        uint64_t total_fees
    );
    std::string calculateMerkleRoot(const std::vector<Transaction>& transactions);
    uint64_t calculateBlockWeight(const std::vector<Transaction>& transactions) const;
    size_t calculateBlockSize(const std::vector<Transaction>& transactions) const;

private:
    ChainDB* chain_db_;  // ChainDB for persistence (also provides chain tip via getTip())
    // Phase 39: chain_manager_ removed (ChainManager deleted - use chain_db_->getTip() instead)
    class Mempool* mempool_;  // Mempool for transaction selection
    BlockRelayManager* block_relay_manager_;  // Phase W.1.3: For network-aware mining (optional)

    // Utreexo integration
    consensus::UtreexoForest* utreexo_forest_ = nullptr;  // Utreexo accumulator for proof generation
    std::shared_ptr<consensus::IUTXOProvider> utxo_provider_;   // UTXO provider for spent output data
    consensus::BlockValidator* block_validator_ = nullptr; // Oracle for Utreexo root computation (single source of truth)

    // Mining address for script generation
    std::string mining_address_;

    // Phase W.1.3: Intelligent transaction selection
    bool use_intelligent_selection_;  // Enable context-aware transaction ranking

    // CT selection policy
    std::unique_ptr<mining::CTSelectionPolicy> ct_policy_;  // CT-specific selection rules
    bool ct_enabled_ = true;  // Include CT transactions in templates

    // Phase W.1.4: Incremental template refresh
    uint64_t last_template_time_ms_;  // Timestamp of last template generation
    std::unordered_set<uint256> last_template_txids_;  // Txids in last template
    
    // Current state
    // SupplyState supply_state_;  // TODO: Define SupplyState struct
    DineroAlgoState algo_state_;
    
    // Configuration
    uint32_t max_block_weight_{4000000};  // 4MB max block weight (BIP141, consensus)
    // Phase 8.5: miner-policy VWU cap. 1M VWU ≈ ~190 P2MR spends per
    // block → ~1s worst-case ML-DSA-65 verify on commodity hardware.
    // Consensus rule stays on max_block_weight_; this is template-only.
    uint64_t max_block_vwu_{1000000};
    // Phase 8.5 Commit 2: per-PQ-scheme input-count cap. Per-scheme so
    // ML-DSA-65 and (future) FALCON-512 can have independent ceilings
    // matching their verify-time profiles. Keyed by scheme_id byte.
    // Missing entries default to "no cap" (uint32_t max).
    std::unordered_map<uint8_t, uint32_t> max_p2mr_inputs_per_scheme_{
        {0x01, 200}  // ML-DSA-65: 200 inputs ≈ 1.4 s worst-case verify
    };
    uint64_t min_tx_fee_{1000};           // Minimum fee per transaction
    uint32_t max_block_time_{7200};       // 2 hours max future time
    
    // Statistics (Stratum)
    mutable std::atomic<uint64_t> jobs_created_{0};
    mutable std::atomic<uint64_t> jobs_refreshed_{0};
    mutable std::atomic<uint32_t> last_job_height_{0};

    // Job tracking (Stratum)
    std::string last_job_id_;
    uint32_t job_counter_{0};

    // v0.14.0.1: Block template statistics (RPC mining)
    BlockTemplateStats block_template_stats_;
    std::string last_template_error_;
};

} // namespace dinero
