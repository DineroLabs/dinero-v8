#pragma once

/**
 * @file prune_service.h
 * @brief Phase 34.8: Prune Mode Service
 *
 * Enables mobile-friendly full nodes by pruning old block data while
 * maintaining full validation capability through Utreexo proofs.
 *
 * With Utreexo integration (Phases 34.1-34.7), pruning is now trivial:
 * - Stateless validation works (proofs carry UTXO existence)
 * - Mempool works without full chainstate
 * - Blocks carry proofs for validation
 * - Only headers needed for chain structure
 *
 * Storage modes:
 * - Full: All blocks stored (default)
 * - Prune: Only recent N blocks + all headers
 * - Headers-only: Only headers (mobile mode)
 *
 * This unlocks:
 * - Mobile full nodes (< 1GB storage)
 * - Raspberry Pi nodes
 * - Embedded systems
 */

#include "daemon/iservice.h"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace dinero {

// Forward declaration for CBlockIndex (defined in consensus/block_index.h)
class CBlockIndex;

namespace daemon {

// Forward declarations
class ChainstateService;
class FastSyncService;

/**
 * @brief Prune mode configuration
 */
struct PruneConfig {
    // Enable prune mode
    bool enabled{false};

    // Number of recent blocks to keep (0 = headers-only mode)
    uint32_t keep_blocks{288};  // ~2 days of blocks at 10min/block

    // Minimum disk space to maintain (in MB)
    uint64_t min_disk_space_mb{550};

    // Auto-prune when disk space is low
    bool auto_prune{true};

    // Keep all headers (required for chain validation)
    bool keep_all_headers{true};

    // Prune block indexes (aggressive mode)
    bool prune_indexes{false};

    // Target storage size (in MB, 0 = no limit)
    uint64_t target_size_mb{0};
};

/**
 * @brief Prune statistics
 */
struct PruneStats {
    uint64_t blocks_pruned{0};
    uint64_t bytes_pruned{0};
    uint64_t headers_kept{0};
    uint64_t blocks_kept{0};
    uint64_t current_disk_usage_mb{0};
    uint32_t lowest_block_height{0};
    uint32_t highest_block_height{0};
    bool is_pruned{false};
};

/**
 * @brief Prune operation result (Phase P.2)
 *
 * Returned by pruneToHeight() to report what was pruned.
 */
struct PruneResult {
    uint32_t blocks_attempted{0};    // Number of blocks checked for pruning
    uint32_t blocks_pruned{0};       // Number of blocks actually deleted
    uint32_t blocks_failed{0};       // Number of blocks that failed to prune
    uint64_t bytes_recovered{0};     // Total bytes freed (block + undo data)
    std::vector<std::string> errors; // Error messages (if any)

    bool success() const { return blocks_failed == 0; }
};

/**
 * @brief Phase 34.8: Prune Mode Service
 *
 * Manages block pruning for space-constrained nodes.
 * Leverages Utreexo proofs for stateless validation after pruning.
 */
class PruneService : public IService {
public:
    PruneService();
    ~PruneService() override;

    // IService interface
    std::string Name() const override { return "PruneService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // ───────────────────────────────────────────────────────────────────────
    // Configuration
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Set prune configuration
     */
    void setConfig(const PruneConfig& config);

    /**
     * @brief Get current configuration
     */
    PruneConfig getConfig() const;

    /**
     * @brief Enable/disable prune mode
     */
    void setEnabled(bool enabled);
    bool isEnabled() const { return config_.enabled; }

    /**
     * @brief Set number of blocks to keep
     * @param blocks Number of recent blocks (0 = headers-only)
     */
    void setKeepBlocks(uint32_t blocks);
    uint32_t getKeepBlocks() const { return config_.keep_blocks; }

    // ───────────────────────────────────────────────────────────────────────
    // Pruning Operations
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Phase P.2: Prune blocks up to a specific height
     *
     * Deletes block and undo data for all blocks below target_height that are
     * marked BLOCK_PRUNE_ELIGIBLE. This is the main pruning entry point.
     *
     * Safety:
     *   - Only prunes blocks with BLOCK_PRUNE_ELIGIBLE flag set
     *   - Re-validates eligibility via ChainManager::ComputePruneEligibility()
     *   - Enforces MIN_BLOCKS_TO_KEEP (288) safety margin
     *   - Thread-safe and idempotent
     *
     * @param target_height Prune all eligible blocks below this height
     * @return PruneResult with stats (blocks_pruned, bytes_recovered, errors)
     */
    PruneResult pruneToHeight(uint32_t target_height);

    /**
     * @brief Trigger pruning if conditions are met (call after block acceptance)
     *
     * This is the main entry point for automatic pruning after a new block
     * is accepted. It runs asynchronously to avoid blocking block processing.
     * Gets the tip height internally from ChainManager.
     */
    void triggerPruneIfNeeded();

    /**
     * @brief Trigger pruning if conditions are met (call after block acceptance)
     *
     * Overload that accepts a known tip height to avoid additional lookups.
     *
     * @param new_tip_height The height of the newly accepted tip
     */
    void triggerPruneIfNeeded(uint32_t new_tip_height);

    /**
     * @brief Prune blocks older than keep_blocks
     * @return Number of blocks pruned
     */
    uint64_t pruneOldBlocks();

    /**
     * @brief Prune to target disk space
     * @param target_mb Target disk usage in MB
     * @return Number of blocks pruned
     */
    uint64_t pruneToTarget(uint64_t target_mb);

    /**
     * @brief Prune specific block range
     * @param start_height First block to prune
     * @param end_height Last block to prune
     * @return Number of blocks pruned
     */
    uint64_t pruneRange(uint32_t start_height, uint32_t end_height);

    /**
     * @brief Check if a block is pruned
     * @param height Block height
     * @return true if block data has been pruned
     */
    bool isBlockPruned(uint32_t height) const;

    /**
     * @brief Check if we can serve a block to peers
     * @param height Block height
     * @return true if we have full block data
     */
    bool canServeBlock(uint32_t height) const;

    // ───────────────────────────────────────────────────────────────────────
    // Headers-Only Mode
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Enable headers-only mode (mobile mode)
     *
     * In this mode:
     * - Only block headers are stored
     * - All validation uses Utreexo proofs
     * - Cannot serve full blocks to peers
     * - Minimal storage requirements
     */
    void enableHeadersOnlyMode();

    /**
     * @brief Check if in headers-only mode
     */
    bool isHeadersOnlyMode() const { return config_.keep_blocks == 0; }

    // ───────────────────────────────────────────────────────────────────────
    // Statistics
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get pruning statistics
     */
    PruneStats getStats() const;

    /**
     * @brief Get stats as formatted string
     */
    std::string getStatsString() const;

    /**
     * @brief Get current disk usage
     * @return Disk usage in bytes
     */
    uint64_t getDiskUsage() const;

    /**
     * @brief Get estimated storage savings
     * @return Bytes saved by pruning
     */
    uint64_t getSavedSpace() const { return stats_.bytes_pruned; }

    // ───────────────────────────────────────────────────────────────────────
    // Safety Guards (Phase P.2)
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Signal that a reorg is starting (called by ChainstateService)
     *
     * Pruning is blocked while reorg is in progress to prevent:
     * - Pruning blocks that may be reconnected after reorg
     * - Race conditions with block disconnect/connect
     */
    void setReorgInProgress(bool in_progress) { reorg_in_progress_ = in_progress; }
    bool isReorgInProgress() const { return reorg_in_progress_; }

    /**
     * @brief Signal that a database flush is in progress
     *
     * Pruning is blocked during flush to prevent:
     * - Corruption from concurrent RocksDB operations
     * - Inconsistent state between memory and disk
     */
    void setFlushInProgress(bool in_progress) { flush_in_progress_ = in_progress; }
    bool isFlushInProgress() const { return flush_in_progress_; }

    /**
     * @brief Check if pruning is currently safe to perform
     *
     * Returns false if:
     * - Node is in Initial Block Download (IBD)
     * - A reorg is in progress
     * - A database flush is in progress
     * - Pruning is already in progress
     */
    bool isPruningSafe() const;

private:
    mutable std::mutex mutex_;
    DaemonContext* ctx_{nullptr};

    // Configuration
    PruneConfig config_;

    // Statistics
    mutable PruneStats stats_;

    // Pruning state
    std::atomic<bool> pruning_in_progress_{false};
    uint32_t prune_height_{0};  // Lowest unpruned block
    std::atomic<uint32_t> last_pruned_height_{0};  // For triggerPruneIfNeeded throttling

    // Phase P.2: Mode immutability
    // Once prune mode is set (either enabled or disabled), it cannot be changed.
    // This prevents accidental data loss from switching modes.
    bool mode_locked_{false};      // True after first Init() or setEnabled()
    bool mode_initialized_{false}; // True if mode was loaded from DB

    // Phase P.2: Safety guards - prevent pruning during critical operations
    std::atomic<bool> reorg_in_progress_{false};  // Set by chainstate during reorg
    std::atomic<bool> flush_in_progress_{false};  // Set by ChainDB during flush

    // Internal methods
    bool pruneBlock(uint32_t height);
    bool pruneBlockFile(const std::string& filename);
    uint64_t calculateDiskUsage() const;
    void updateStats();
    bool shouldAutoPrune() const;
    void runAutoPrune();

    // Phase P.2: Persistence helpers
    void loadPruneHeight();       // Load from ChainDB on startup
    void persistPruneHeight();    // Save to ChainDB after pruning
    void persistBlockIndexFlags(const dinero::CBlockIndex* pindex);  // Update flags in ChainDB
    void loadPruneMode();         // Load and lock mode from ChainDB
    void persistPruneMode();      // Save mode to ChainDB (only if not already set)

    // Phase P.2: Block index invariants
    void clearBlockDataFlags(uint32_t from_height, uint32_t to_height);  // Clear BLOCK_HAVE_DATA for pruned range
    void verifyBlockIndexConsistency();  // Verify flags match file existence on startup
};

} // namespace daemon
} // namespace dinero
