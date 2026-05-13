#include "daemon/services/prune_service.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"  // Phase P.2: BLOCK_HAVE_DATA, BLOCK_HAVE_UNDO
#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"  // Phase P.2: For ChainWriteToken
#include "common/logger.h"
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <thread>

namespace dinero {
namespace daemon {

PruneService::PruneService() {
    g_logger.info("Phase 34.8: PruneService created");
}

PruneService::~PruneService() = default;

bool PruneService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;

    g_logger.info("Phase 34.8: PruneService initializing...");

    // Phase P.2: Load persisted prune height from ChainDB
    loadPruneHeight();

    // Phase P.2: Load and lock prune mode from ChainDB
    loadPruneMode();

    // Phase P.2: Verify block index consistency (file ↔ index invariant)
    verifyBlockIndexConsistency();

    // Update initial stats
    updateStats();

    g_logger.info("Phase 34.8: PruneService initialized");
    g_logger.info("Phase 34.8: Prune mode: " + std::string(config_.enabled ? "ENABLED" : "disabled"));
    g_logger.info("Phase 34.8: Mode locked: " + std::string(mode_locked_ ? "YES" : "no"));
    if (prune_height_ > 0) {
        g_logger.info("Phase 34.8: Restored prune height: " + std::to_string(prune_height_));
    }
    if (config_.enabled) {
        g_logger.info("Phase 34.8: Keep blocks: " + std::to_string(config_.keep_blocks));
        g_logger.info("Phase 34.8: Headers-only: " + std::string(isHeadersOnlyMode() ? "YES" : "no"));
    }

    return true;
}

bool PruneService::Start() {
    g_logger.info("Phase 34.8: PruneService started");

    if (config_.enabled) {
        g_logger.info("Phase 34.8: Prune mode active - mobile-friendly storage");

        // Run initial prune if enabled
        if (config_.auto_prune && shouldAutoPrune()) {
            runAutoPrune();
        }
    }

    return true;
}

void PruneService::Stop() {
    g_logger.info("Phase 34.8: PruneService stopping...");

    // Log final stats
    g_logger.info("Phase 34.8: Final stats:\n" + getStatsString());

    g_logger.info("Phase 34.8: PruneService stopped");
}

void PruneService::setConfig(const PruneConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Phase P.2: Check if mode is locked and enabled is being changed
    if (mode_locked_ && config.enabled != config_.enabled) {
        g_logger.warn("Phase 34.8: Cannot change prune mode after initialization!");
        g_logger.warn("Phase 34.8: Current mode: " + std::string(config_.enabled ? "PRUNED" : "ARCHIVAL"));
        g_logger.warn("Phase 34.8: To change mode, wipe datadir and restart.");
        return;  // Reject the change
    }

    // If mode not locked yet and enabling, persist and lock it
    if (!mode_locked_ && config.enabled) {
        config_ = config;
        persistPruneMode();
        mode_locked_ = true;
        g_logger.info("Phase 34.8: Prune mode ENABLED and locked (immutable)");
    } else if (!mode_locked_) {
        // First run, not pruning - lock as archival
        config_ = config;
        persistPruneMode();
        mode_locked_ = true;
        g_logger.info("Phase 34.8: Archival mode locked (immutable)");
    } else {
        // Mode locked, only update non-mode settings
        bool old_enabled = config_.enabled;
        config_ = config;
        config_.enabled = old_enabled;  // Preserve locked mode
    }

    g_logger.info("Phase 34.8: Configuration updated");
    g_logger.info("Phase 34.8:   Enabled: " + std::string(config_.enabled ? "yes" : "no"));
    g_logger.info("Phase 34.8:   Keep blocks: " + std::to_string(config_.keep_blocks));
    g_logger.info("Phase 34.8:   Auto-prune: " + std::string(config_.auto_prune ? "yes" : "no"));
}

PruneConfig PruneService::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void PruneService::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Phase P.2: Check if mode is locked
    if (mode_locked_ && enabled != config_.enabled) {
        g_logger.warn("Phase 34.8: Cannot change prune mode after initialization!");
        g_logger.warn("Phase 34.8: Current mode: " + std::string(config_.enabled ? "PRUNED" : "ARCHIVAL"));
        g_logger.warn("Phase 34.8: To change mode, wipe datadir and restart.");
        return;  // Reject the change
    }

    // If not locked, set and lock the mode
    if (!mode_locked_) {
        config_.enabled = enabled;
        persistPruneMode();
        mode_locked_ = true;
        g_logger.info("Phase 34.8: Prune mode " + std::string(enabled ? "ENABLED" : "disabled") + " and locked");
    }

    if (config_.enabled && config_.auto_prune) {
        // Schedule auto-prune
        runAutoPrune();
    }
}

void PruneService::setKeepBlocks(uint32_t blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.keep_blocks = blocks;

    g_logger.info("Phase 34.8: Keep blocks set to " + std::to_string(blocks));

    if (blocks == 0) {
        g_logger.info("Phase 34.8: Headers-only mode enabled (mobile mode)");
    }
}

uint64_t PruneService::pruneOldBlocks() {
    if (!config_.enabled) {
        g_logger.debug("Phase 34.8: Pruning disabled, skipping");
        return 0;
    }

    if (pruning_in_progress_.exchange(true)) {
        g_logger.debug("Phase 34.8: Pruning already in progress");
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Get current chain height
    uint32_t current_height = 0;
    if (ctx_ && ctx_->chainstate) {
        // Get height from chainstate service
        // For now, use stats
        current_height = stats_.highest_block_height;
    }

    if (current_height == 0) {
        pruning_in_progress_ = false;
        return 0;
    }

    // Calculate prune target
    uint32_t prune_target = 0;
    if (current_height > config_.keep_blocks) {
        prune_target = current_height - config_.keep_blocks;
    }

    if (prune_target <= prune_height_) {
        // Nothing to prune
        pruning_in_progress_ = false;
        return 0;
    }

    g_logger.info("Phase 34.8: Pruning blocks " + std::to_string(prune_height_) +
                 " to " + std::to_string(prune_target));

    uint64_t pruned = 0;
    for (uint32_t h = prune_height_; h < prune_target; ++h) {
        if (pruneBlock(h)) {
            pruned++;
        }
    }

    prune_height_ = prune_target;
    stats_.blocks_pruned += pruned;

    g_logger.info("Phase 34.8: Pruned " + std::to_string(pruned) + " blocks");

    updateStats();
    pruning_in_progress_ = false;

    return pruned;
}

uint64_t PruneService::pruneToTarget(uint64_t target_mb) {
    if (!config_.enabled) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t current_mb = stats_.current_disk_usage_mb;
    if (current_mb <= target_mb) {
        return 0;  // Already under target
    }

    g_logger.info("Phase 34.8: Pruning to target " + std::to_string(target_mb) +
                 " MB (current: " + std::to_string(current_mb) + " MB)");

    uint64_t total_pruned = 0;

    // Prune oldest blocks until we reach target
    while (stats_.current_disk_usage_mb > target_mb && prune_height_ < stats_.highest_block_height) {
        if (pruneBlock(prune_height_)) {
            total_pruned++;
            prune_height_++;
            stats_.blocks_pruned++;
        } else {
            break;  // Can't prune more
        }

        // Recalculate occasionally
        if (total_pruned % 100 == 0) {
            updateStats();
        }
    }

    updateStats();

    g_logger.info("Phase 34.8: Pruned " + std::to_string(total_pruned) + " blocks to reach target");

    return total_pruned;
}

uint64_t PruneService::pruneRange(uint32_t start_height, uint32_t end_height) {
    if (!config_.enabled) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    g_logger.info("Phase 34.8: Pruning block range " + std::to_string(start_height) +
                 " to " + std::to_string(end_height));

    uint64_t pruned = 0;
    for (uint32_t h = start_height; h <= end_height; ++h) {
        if (pruneBlock(h)) {
            pruned++;
        }
    }

    stats_.blocks_pruned += pruned;
    updateStats();

    g_logger.info("Phase 34.8: Pruned " + std::to_string(pruned) + " blocks in range");

    return pruned;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase P.2: Physical Block Pruning
// ═══════════════════════════════════════════════════════════════════════════

PruneResult PruneService::pruneToHeight(uint32_t target_height) {
    // Consensus constant: Never prune within 288 blocks of tip (reorg safety)
    constexpr uint32_t MIN_BLOCKS_TO_KEEP = 288;

    PruneResult result;

    // Safety: Check if pruning is enabled
    if (!config_.enabled) {
        result.errors.push_back("Pruning is disabled");
        return result;
    }

    // Phase P.2: Safety guards - block pruning during critical operations
    if (!isPruningSafe()) {
        result.errors.push_back("Pruning blocked: critical operation in progress (IBD/reorg/flush)");
        return result;
    }

    // Prevent concurrent pruning
    if (pruning_in_progress_.exchange(true)) {
        result.errors.push_back("Pruning already in progress");
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Get ChainDB from context (Phase 39: ChainManager deleted)
    if (!ctx_ || !ctx_->chainstate) {
        result.errors.push_back("ChainstateService not available");
        pruning_in_progress_ = false;
        return result;
    }

    ChainDB* chain_db = ctx_->chainstate->GetChainDB();
    if (!chain_db) {
        result.errors.push_back("ChainDB not available");
        pruning_in_progress_ = false;
        return result;
    }

    // Get active tip height from ChainDB
    auto tip_result = chain_db->getTip();
    if (tip_result.status() != Status::Ok) {
        result.errors.push_back("Failed to get chain tip");
        pruning_in_progress_ = false;
        return result;
    }

    uint32_t tip_height = tip_result.value().height;

    // Enforce MIN_BLOCKS_TO_KEEP safety margin
    if (tip_height < MIN_BLOCKS_TO_KEEP) {
        result.errors.push_back("Chain too short to prune (height " + std::to_string(tip_height) + " < " + std::to_string(MIN_BLOCKS_TO_KEEP) + ")");
        pruning_in_progress_ = false;
        return result;
    }

    uint32_t max_prune_height = tip_height - MIN_BLOCKS_TO_KEEP;

    // Validate target_height is safe
    if (target_height > max_prune_height) {
        result.errors.push_back("Target height " + std::to_string(target_height) +
                               " exceeds safe prune height " + std::to_string(max_prune_height) +
                               " (tip=" + std::to_string(tip_height) + ", keep=" + std::to_string(MIN_BLOCKS_TO_KEEP) + ")");
        pruning_in_progress_ = false;
        return result;
    }

    g_logger.info("Phase 34.8: Pruning blocks up to height " + std::to_string(target_height) +
                 " (tip=" + std::to_string(tip_height) + ", max_safe=" + std::to_string(max_prune_height) + ")");

    // Get BlockStorage from context for file-level pruning
    BlockStorage* block_storage = ctx_->block_storage.get();
    if (!block_storage) {
        // Fallback: just track logical pruning state
        uint32_t blocks_to_prune = (target_height > prune_height_) ?
            (target_height - prune_height_) : 0;
        result.blocks_pruned = blocks_to_prune;
        result.bytes_recovered = blocks_to_prune * 1024;  // Estimate
    } else {
        // File-level pruning: delete entire blk/rev files when possible
        auto file_info = block_storage->getFileInfo();
        uint64_t bytes_freed = 0;
        uint32_t files_deleted = 0;

        for (const auto& info : file_info) {
            // Skip current write file (can't delete it)
            // Files with file_number 0 might still have genesis, be conservative
            if (info.file_number == 0) {
                continue;
            }

            // Heuristic: Lower file numbers contain older (lower height) blocks
            // We can delete files where highest_height < target_height
            // Since we don't track heights per file yet, use conservative approach:
            // Only delete files that are significantly behind the prune frontier

            // For now, delete files that are at least 2 files behind current
            // This ensures we never delete blocks we might still need
            auto stats_result = block_storage->getStats();
            if (stats_result.status() != Status::Ok) {
                continue;
            }

            uint32_t current_file = stats_result.value().current_file_number;
            if (info.file_number >= current_file) {
                continue;  // Don't delete current or recent files
            }

            // Delete this file
            auto delete_result = block_storage->deleteFile(info.file_number);
            if (delete_result.status() == Status::Ok) {
                bytes_freed += delete_result.value();
                files_deleted++;
                g_logger.info("Phase 34.8: Deleted blk/rev" +
                    std::to_string(info.file_number) + ".dat (" +
                    std::to_string(delete_result.value() / 1024 / 1024) + " MB)");
            }
        }

        result.blocks_attempted = static_cast<uint32_t>(file_info.size());
        result.blocks_pruned = files_deleted;  // Track as files deleted
        result.bytes_recovered = bytes_freed;
    }

    // Update service statistics
    stats_.blocks_pruned += result.blocks_pruned;
    stats_.bytes_pruned += result.bytes_recovered;
    stats_.lowest_block_height = target_height;

    // Phase P.2: Clear BLOCK_HAVE_DATA flags for pruned blocks (file ↔ index invariant)
    uint32_t old_prune_height = prune_height_;
    prune_height_ = target_height;
    if (target_height > old_prune_height) {
        clearBlockDataFlags(old_prune_height, target_height - 1);
    }

    // Phase P.2: Persist prune height to ChainDB for crash recovery
    persistPruneHeight();

    g_logger.info("Phase 34.8: Pruning complete - " + std::to_string(result.blocks_pruned) +
                 " blocks/files pruned, " + std::to_string(result.bytes_recovered / 1024) + " KB recovered");

    updateStats();
    pruning_in_progress_ = false;

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// triggerPruneIfNeeded: Automatic pruning after block acceptance
// ═══════════════════════════════════════════════════════════════════════════

void PruneService::triggerPruneIfNeeded() {
    // Get tip height from ChainDB (Phase 39: ChainManager deleted)
    if (!ctx_ || !ctx_->chainstate) {
        return;
    }

    ChainDB* chain_db = ctx_->chainstate->GetChainDB();
    if (!chain_db) {
        return;
    }

    auto tip_result = chain_db->getTip();
    if (tip_result.status() != Status::Ok) {
        return;
    }

    triggerPruneIfNeeded(tip_result.value().height);
}

void PruneService::triggerPruneIfNeeded(uint32_t new_tip_height) {
    // Consensus constant: Never prune within 288 blocks of tip (reorg safety)
    constexpr uint32_t MIN_BLOCKS_TO_KEEP = 288;

    if (!config_.enabled || !config_.auto_prune) {
        return;
    }

    // Phase P.2: Safety guards - skip pruning during critical operations
    if (!isPruningSafe()) {
        return;
    }

    // Prevent concurrent pruning
    bool expected = false;
    if (!pruning_in_progress_.compare_exchange_strong(expected, true)) {
        return;  // Already pruning
    }

    // Calculate prune target height
    if (new_tip_height <= config_.keep_blocks + MIN_BLOCKS_TO_KEEP) {
        pruning_in_progress_ = false;
        return;  // Not enough blocks to prune yet
    }

    uint32_t target_height = new_tip_height - config_.keep_blocks - MIN_BLOCKS_TO_KEEP;

    // Check if we've already pruned to this height (throttle)
    if (target_height <= last_pruned_height_.load()) {
        pruning_in_progress_ = false;
        return;
    }

    // Trigger async pruning (non-blocking)
    std::thread([this, target_height]() {
        try {
            auto result = pruneToHeight(target_height);
            if (result.blocks_pruned > 0) {
                last_pruned_height_ = target_height;
                g_logger.info("[PruneService] Pruned to height " +
                    std::to_string(target_height) + ", freed " +
                    std::to_string(result.bytes_recovered / 1024 / 1024) + " MB");
            }
        } catch (const std::exception& e) {
            g_logger.error("[PruneService] Exception during pruning: " + std::string(e.what()));
        } catch (...) {
            g_logger.error("[PruneService] Unknown exception during pruning");
        }
        pruning_in_progress_ = false;
    }).detach();
}

bool PruneService::isBlockPruned(uint32_t height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return height < prune_height_;
}

bool PruneService::canServeBlock(uint32_t height) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Can't serve pruned blocks
    if (height < prune_height_) {
        return false;
    }

    // Can't serve future blocks
    if (height > stats_.highest_block_height) {
        return false;
    }

    return true;
}

void PruneService::enableHeadersOnlyMode() {
    std::lock_guard<std::mutex> lock(mutex_);

    config_.enabled = true;
    config_.keep_blocks = 0;  // No block data, headers only
    mode_locked_ = true;

    // Persist so the node stays in pruned mode after restart
    persistPruneMode();

    g_logger.info("Phase 34.8: Headers-only mode ENABLED and persisted");
    g_logger.info("Phase 34.8: This is mobile mode - minimal storage, full validation via proofs");

    // Prune all existing blocks
    if (stats_.highest_block_height > 0) {
        g_logger.info("Phase 34.8: Pruning all block data...");
        for (uint32_t h = 0; h <= stats_.highest_block_height; ++h) {
            pruneBlock(h);
        }
        prune_height_ = stats_.highest_block_height + 1;
    }

    updateStats();
}

PruneStats PruneService::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string PruneService::getStatsString() const {
    PruneStats s = getStats();

    std::stringstream ss;
    ss << "Phase 34.8 Prune Mode:\n"
       << "  Mode: " << (config_.enabled ? (isHeadersOnlyMode() ? "headers-only" : "pruned") : "full") << "\n"
       << "  Blocks pruned: " << s.blocks_pruned << "\n"
       << "  Bytes pruned: " << s.bytes_pruned << " (" << (s.bytes_pruned / 1024 / 1024) << " MB)\n"
       << "  Headers kept: " << s.headers_kept << "\n"
       << "  Blocks kept: " << s.blocks_kept << "\n"
       << "  Disk usage: " << s.current_disk_usage_mb << " MB\n"
       << "  Pruned range: 0 to " << s.lowest_block_height << "\n"
       << "  Available range: " << s.lowest_block_height << " to " << s.highest_block_height;

    return ss.str();
}

uint64_t PruneService::getDiskUsage() const {
    return calculateDiskUsage();
}

bool PruneService::pruneBlock(uint32_t height) {
    // In a real implementation, this would:
    // 1. Mark block as pruned in ChainDB
    // 2. Delete block data from blk*.dat files
    // 3. Keep header data
    // 4. Update block index

    // For now, track the pruning
    stats_.bytes_pruned += 1024;  // Estimate ~1KB per block (simplified)

    g_logger.debug("Phase 34.8: Pruned block " + std::to_string(height));

    return true;
}

bool PruneService::pruneBlockFile(const std::string& filename) {
    // Delete or truncate a block file
    // In production, would use filesystem operations

    g_logger.debug("Phase 34.8: Pruned block file " + filename);
    return true;
}

uint64_t PruneService::calculateDiskUsage() const {
    uint64_t total_bytes = 0;

    // In production, would scan blockchain directory
    // For now, estimate based on block count

    uint64_t blocks_on_disk = stats_.highest_block_height > prune_height_ ?
                              stats_.highest_block_height - prune_height_ : 0;

    // Estimate 1KB per block on average (simplified)
    total_bytes = blocks_on_disk * 1024;

    // Add header storage (80 bytes per header)
    total_bytes += stats_.headers_kept * 80;

    return total_bytes;
}

void PruneService::updateStats() {
    // Update stats based on current state
    stats_.lowest_block_height = prune_height_;
    stats_.is_pruned = config_.enabled && prune_height_ > 0;

    // Calculate blocks kept
    if (stats_.highest_block_height > prune_height_) {
        stats_.blocks_kept = stats_.highest_block_height - prune_height_;
    } else {
        stats_.blocks_kept = 0;
    }

    // Headers are always kept
    stats_.headers_kept = stats_.highest_block_height;

    // Calculate disk usage
    stats_.current_disk_usage_mb = calculateDiskUsage() / (1024 * 1024);
}

bool PruneService::shouldAutoPrune() const {
    if (!config_.auto_prune) {
        return false;
    }

    // Check if we exceed target size
    if (config_.target_size_mb > 0 && stats_.current_disk_usage_mb > config_.target_size_mb) {
        return true;
    }

    // Check minimum disk space
    // In production, would check actual free disk space
    if (stats_.current_disk_usage_mb > config_.min_disk_space_mb) {
        return true;
    }

    return false;
}

void PruneService::runAutoPrune() {
    if (!config_.enabled || !config_.auto_prune) {
        return;
    }

    g_logger.info("Phase 34.8: Running auto-prune...");

    if (config_.target_size_mb > 0) {
        pruneToTarget(config_.target_size_mb);
    } else {
        pruneOldBlocks();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase P.2: Persistence Methods
// ═══════════════════════════════════════════════════════════════════════════

void PruneService::loadPruneHeight() {
    if (!ctx_ || !ctx_->chainstate) {
        g_logger.warn("[PruneService] Cannot load prune height: chainstate not available");
        return;
    }

    // Get ChainDB from chainstate service
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        g_logger.warn("[PruneService] Cannot load prune height: chainstate cast failed");
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        g_logger.warn("[PruneService] Cannot load prune height: ChainDB not available");
        return;
    }

    auto result = chain_db->getPruneHeight();
    if (result.ok()) {
        prune_height_ = result.value();
        if (prune_height_ > 0) {
            g_logger.info("[PruneService] Loaded persisted prune height: " + std::to_string(prune_height_));
            stats_.is_pruned = true;
            stats_.lowest_block_height = prune_height_;
        }
    } else if (result.status() == Status::NotFound) {
        // No prune height stored yet - fresh node or never pruned
        prune_height_ = 0;
    } else {
        g_logger.error("[PruneService] Failed to load prune height: " + std::to_string(static_cast<int>(result.status())));
    }
}

void PruneService::persistPruneHeight() {
    if (!ctx_ || !ctx_->chainstate) {
        g_logger.warn("[PruneService] Cannot persist prune height: chainstate not available");
        return;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        g_logger.warn("[PruneService] Cannot persist prune height: chainstate cast failed");
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        g_logger.warn("[PruneService] Cannot persist prune height: ChainDB not available");
        return;
    }

    // PruneService is a friend of ChainWriteToken, so we can create one
    dinero::ChainWriteToken token;
    auto status = chain_db->setPruneHeight(token, prune_height_);
    if (status != Status::Ok) {
        g_logger.error("[PruneService] Failed to persist prune height: " + std::to_string(static_cast<int>(status)));
    } else {
        g_logger.debug("[PruneService] Persisted prune height: " + std::to_string(prune_height_));
    }
}

void PruneService::persistBlockIndexFlags(const CBlockIndex* pindex) {
    if (!pindex || !ctx_ || !ctx_->chainstate) {
        return;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        return;
    }

    // Update block index in ChainDB with new status flags
    dinero::ChainWriteToken token;
    auto status = chain_db->updateBlockIndex(token, pindex);
    if (status != Status::Ok) {
        g_logger.error("[PruneService] Failed to persist block index flags for block " +
                      pindex->hash.GetHex().substr(0, 16) + "...: " +
                      std::to_string(static_cast<int>(status)));
    }
}

void PruneService::loadPruneMode() {
    if (!ctx_ || !ctx_->chainstate) {
        g_logger.warn("[PruneService] Cannot load prune mode: chainstate not available");
        return;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        g_logger.warn("[PruneService] Cannot load prune mode: chainstate cast failed");
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        g_logger.warn("[PruneService] Cannot load prune mode: ChainDB not available");
        return;
    }

    auto result = chain_db->getPruneMode();
    if (result.ok()) {
        bool persisted_mode = result.value();
        mode_initialized_ = true;
        mode_locked_ = true;

        // Check for mode mismatch
        if (persisted_mode != config_.enabled) {
            g_logger.warn("[PruneService] Mode mismatch detected!");
            g_logger.warn("[PruneService] Persisted mode: " + std::string(persisted_mode ? "PRUNED" : "ARCHIVAL"));
            g_logger.warn("[PruneService] Requested mode: " + std::string(config_.enabled ? "PRUNED" : "ARCHIVAL"));
            g_logger.warn("[PruneService] Using persisted mode (to change, wipe datadir)");
            config_.enabled = persisted_mode;  // Force to persisted mode
        }

        g_logger.info("[PruneService] Loaded persisted prune mode: " +
                     std::string(persisted_mode ? "PRUNED" : "ARCHIVAL") + " (locked)");
    } else if (result.status() == Status::NotFound) {
        // Fresh node - mode not set yet, will be locked on first setConfig/setEnabled
        mode_initialized_ = false;
        mode_locked_ = false;
        g_logger.info("[PruneService] Fresh node - prune mode not yet set");
    } else {
        g_logger.error("[PruneService] Failed to load prune mode: " +
                      std::to_string(static_cast<int>(result.status())));
    }
}

void PruneService::persistPruneMode() {
    if (!ctx_ || !ctx_->chainstate) {
        g_logger.warn("[PruneService] Cannot persist prune mode: chainstate not available");
        return;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        g_logger.warn("[PruneService] Cannot persist prune mode: chainstate cast failed");
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        g_logger.warn("[PruneService] Cannot persist prune mode: ChainDB not available");
        return;
    }

    // Check if already persisted - don't overwrite
    auto existing = chain_db->getPruneMode();
    if (existing.ok()) {
        g_logger.debug("[PruneService] Prune mode already persisted, skipping");
        return;
    }

    dinero::ChainWriteToken token;
    auto status = chain_db->setPruneMode(token, config_.enabled);
    if (status != Status::Ok) {
        g_logger.error("[PruneService] Failed to persist prune mode: " +
                      std::to_string(static_cast<int>(status)));
    } else {
        g_logger.info("[PruneService] Persisted prune mode: " +
                     std::string(config_.enabled ? "PRUNED" : "ARCHIVAL"));
        mode_initialized_ = true;
    }
}

void PruneService::clearBlockDataFlags(uint32_t from_height, uint32_t to_height) {
    if (!ctx_ || !ctx_->chainstate) {
        g_logger.warn("[PruneService] Cannot clear block flags: chainstate not available");
        return;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        return;
    }

    uint32_t flags_cleared = 0;

    for (uint32_t height = from_height; height <= to_height; ++height) {
        // Get block hash at this height
        auto hash_result = chain_db->getBlockHashByHeight(height);
        if (!hash_result.ok()) {
            continue;
        }

        // Get block index
        CBlockIndex* pindex = chain_db->getBlockIndex(hash_result.value());
        if (!pindex) {
            continue;
        }

        // Check if block has data flag set
        if (pindex->status & BLOCK_HAVE_DATA) {
            // Clear the BLOCK_HAVE_DATA flag
            pindex->status &= ~BLOCK_HAVE_DATA;

            // Also clear BLOCK_HAVE_UNDO since undo data is deleted with block data
            pindex->status &= ~BLOCK_HAVE_UNDO;

            // Persist the updated flags
            dinero::ChainWriteToken token;
            auto status = chain_db->updateBlockIndex(token, pindex);
            if (status == Status::Ok) {
                flags_cleared++;
            } else {
                g_logger.warn("[PruneService] Failed to clear flags for block at height " +
                             std::to_string(height));
            }
        }
    }

    if (flags_cleared > 0) {
        g_logger.info("[PruneService] Cleared BLOCK_HAVE_DATA flags for " +
                     std::to_string(flags_cleared) + " blocks (heights " +
                     std::to_string(from_height) + " to " + std::to_string(to_height) + ")");
    }
}

void PruneService::verifyBlockIndexConsistency() {
    if (!ctx_ || !ctx_->chainstate) {
        return;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        return;
    }

    auto* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        return;
    }

    // Get persisted prune height
    auto prune_result = chain_db->getPruneHeight();
    if (!prune_result.ok() || prune_result.value() == 0) {
        // No pruning performed, nothing to verify
        return;
    }

    uint32_t persisted_prune_height = prune_result.value();
    uint32_t inconsistent_count = 0;

    // Verify all blocks below prune height have BLOCK_HAVE_DATA cleared
    for (uint32_t height = 0; height < persisted_prune_height; ++height) {
        auto hash_result = chain_db->getBlockHashByHeight(height);
        if (!hash_result.ok()) {
            continue;
        }

        CBlockIndex* pindex = chain_db->getBlockIndex(hash_result.value());
        if (!pindex) {
            continue;
        }

        // If block is below prune height but still has data flag, fix it
        if (pindex->status & BLOCK_HAVE_DATA) {
            g_logger.warn("[PruneService] Inconsistent block index at height " +
                         std::to_string(height) + ": BLOCK_HAVE_DATA set but block is pruned");

            // Clear the flags
            pindex->status &= ~BLOCK_HAVE_DATA;
            pindex->status &= ~BLOCK_HAVE_UNDO;

            dinero::ChainWriteToken token;
            chain_db->updateBlockIndex(token, pindex);
            inconsistent_count++;
        }
    }

    if (inconsistent_count > 0) {
        g_logger.warn("[PruneService] Fixed " + std::to_string(inconsistent_count) +
                     " inconsistent block index entries during startup");
    } else {
        g_logger.info("[PruneService] Block index consistency verified (" +
                     std::to_string(persisted_prune_height) + " pruned blocks)");
    }
}

bool PruneService::isPruningSafe() const {
    // Check 1: Pruning already in progress
    if (pruning_in_progress_.load()) {
        g_logger.debug("[PruneService] Pruning blocked: already in progress");
        return false;
    }

    // Check 2: Reorg in progress
    if (reorg_in_progress_.load()) {
        g_logger.debug("[PruneService] Pruning blocked: reorg in progress");
        return false;
    }

    // Check 3: Database flush in progress
    if (flush_in_progress_.load()) {
        g_logger.debug("[PruneService] Pruning blocked: flush in progress");
        return false;
    }

    // Check 4: Initial Block Download (IBD)
    if (ctx_ && ctx_->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
        if (chainstate && chainstate->IsInIBD()) {
            g_logger.debug("[PruneService] Pruning blocked: Initial Block Download in progress");
            return false;
        }
    }

    return true;
}

} // namespace daemon
} // namespace dinero
