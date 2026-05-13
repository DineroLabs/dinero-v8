#pragma once

#include "p2p/block_download_scheduler.h"  // For SyncPhase
#include <optional>
#include <chrono>
#include <cstdint>
#include <string>

namespace dinero {

// Forward declarations
class ChainDB;
class Wallet;
class BlockDownloadScheduler;
class HeaderSyncManager;

/**
 * @brief Phase W.2.1: Wallet Sync Status (Foundation)
 *
 * Single authoritative source of truth for wallet sync state.
 * Aggregates signals from Phase G (networking) and existing components.
 *
 * Design Principles:
 * - Truth over smoothness (never lie about progress)
 * - Phase-aware (IBD vs Catching Up vs Steady State)
 * - Telemetry-driven (every field maps to existing metric)
 * - No polling magic, no divorced heuristics
 */
struct WalletSyncStatus {
    // ========================================================================
    // Phase & Overall Progress
    // ========================================================================

    /**
     * @brief Current sync phase (from G.12)
     *
     * Determines how progress is calculated and displayed:
     * - IBD: Headers + blocks weighted
     * - CATCHING_UP: Blocks dominant
     * - STEADY_STATE: Wallet scan dominant
     */
    SyncPhase phase;

    /**
     * @brief Overall progress [0.0, 1.0] (phase-aware)
     *
     * Calculated based on current phase:
     * - IBD: 0.4*headers + 0.4*blocks + 0.2*scan
     * - CATCHING_UP: 0.2*headers + 0.6*blocks + 0.2*scan
     * - STEADY_STATE: 0.1*headers + 0.1*blocks + 0.8*scan
     *
     * Never shows 1.0 unless ALL components are complete.
     */
    double overall_progress;

    /**
     * @brief Estimated time to completion (honest)
     *
     * Rules:
     * - Requires ≥30s window of stable data
     * - If unstable → nullopt ("Estimating...")
     * - If reorg → frozen until recovery complete
     * - Based on rolling average rates
     */
    std::optional<std::chrono::seconds> eta;

    // ========================================================================
    // Header Sync Progress (from HeaderSyncManager)
    // ========================================================================

    /** Headers downloaded and validated */
    uint64_t headers_synced;

    /** Total headers known from peers */
    uint64_t headers_total;

    /** Header sync progress [0.0, 1.0] */
    double headers_progress() const {
        if (headers_total == 0) return 0.0;
        return static_cast<double>(headers_synced) / headers_total;
    }

    // ========================================================================
    // Block Sync Progress (from BlockDownloadScheduler)
    // ========================================================================

    /** Blocks downloaded and validated */
    uint64_t blocks_synced;

    /** Total blocks needed (same as headers_total) */
    uint64_t blocks_total;

    /** Block sync progress [0.0, 1.0] */
    double blocks_progress() const {
        if (blocks_total == 0) return 0.0;
        return static_cast<double>(blocks_synced) / blocks_total;
    }

    // ========================================================================
    // Wallet Scan Progress (from Wallet)
    // ========================================================================

    /** Height wallet has scanned to */
    uint64_t wallet_scan_height;

    /** Current chain tip height */
    uint64_t chain_height;

    /** Wallet scan progress [0.0, 1.0] */
    double wallet_scan_progress() const {
        if (chain_height == 0) return 0.0;
        return static_cast<double>(wallet_scan_height) / chain_height;
    }

    // ========================================================================
    // Reorg State (Phase W.2.4)
    // ========================================================================

    /** True if reorganization is actively being processed */
    bool is_reorg_in_progress;

    /** Depth of last detected reorg (0 if none) */
    int last_reorg_depth;

    // ========================================================================
    // Slow Reason (Phase W.2.5)
    // ========================================================================

    /**
     * @brief Primary reason for slow sync (if any)
     *
     * Answers "Why is it slow?"
     * - NETWORK_IBD: Initial sync, network is syncing
     * - LOW_PEER_QUALITY: Poor peer connections
     * - DISK_BOUND: Disk I/O bottleneck
     * - HIGH_MEMPOOL_PRESSURE: Network congestion
     * - REORG_RECOVERY: Recovering from reorg
     * - WALLET_RESCAN: Scanning wallet history
     * - NONE: Syncing normally
     */
    std::string slow_reason_description;

    /**
     * @brief User-actionable suggestion for slow reason
     *
     * Examples:
     * - "Try restarting the node or adding high-quality peers"
     * - "Consider using faster storage (SSD recommended)"
     * - "This is normal during initial sync. Please wait."
     */
    std::string slow_reason_suggestion;

    // ========================================================================
    // Construction & Validation
    // ========================================================================

    WalletSyncStatus()
        : phase(SyncPhase::STEADY_STATE)
        , overall_progress(0.0)
        , eta(std::nullopt)
        , headers_synced(0)
        , headers_total(0)
        , blocks_synced(0)
        , blocks_total(0)
        , wallet_scan_height(0)
        , chain_height(0)
        , is_reorg_in_progress(false)
        , last_reorg_depth(0)
        , slow_reason_description("")
        , slow_reason_suggestion("")
    {}

    /**
     * @brief Verify sync status consistency
     *
     * Invariants:
     * - headers_synced ≤ headers_total
     * - blocks_synced ≤ blocks_total
     * - wallet_scan_height ≤ chain_height
     * - overall_progress ∈ [0.0, 1.0]
     * - overall_progress = 1.0 ⟹ fully synced
     */
    bool IsValid() const {
        if (headers_synced > headers_total) return false;
        if (blocks_synced > blocks_total) return false;
        if (wallet_scan_height > chain_height) return false;
        if (overall_progress < 0.0 || overall_progress > 1.0) return false;

        // If claiming 100%, must actually be complete
        if (overall_progress >= 0.9999) {
            if (headers_synced < headers_total) return false;
            if (blocks_synced < blocks_total) return false;
            if (wallet_scan_height < chain_height) return false;
        }

        return true;
    }

    /**
     * @brief Check if fully synced
     *
     * Only returns true when ALL components are complete:
     * - Headers synced
     * - Blocks synced
     * - Wallet scan complete
     * - Chain height > 0 (not empty chain)
     */
    bool IsFullySynced() const {
        // Empty chain (chain_height = 0) is not considered synced
        if (chain_height == 0) return false;

        return headers_synced >= headers_total &&
               blocks_synced >= blocks_total &&
               wallet_scan_height >= chain_height &&
               phase == SyncPhase::STEADY_STATE;
    }

    /**
     * @brief Get human-readable phase name
     */
    std::string GetPhaseName() const {
        switch (phase) {
            case SyncPhase::IBD:
                return "Initial Block Download";
            case SyncPhase::CATCHING_UP:
                return "Catching Up";
            case SyncPhase::STEADY_STATE:
                return "Synced";
            default:
                return "Unknown";
        }
    }

    /**
     * @brief Get human-readable status description
     */
    std::string GetStatusDescription() const {
        if (IsFullySynced()) {
            return "Fully synced";
        }

        if (is_reorg_in_progress) {
            return "Reorganization in progress (depth: " +
                   std::to_string(last_reorg_depth) + ")";
        }

        if (phase == SyncPhase::IBD) {
            return "Downloading blockchain";
        }

        if (phase == SyncPhase::CATCHING_UP) {
            return "Catching up to network";
        }

        // STEADY_STATE but not fully synced (wallet scan lagging)
        return "Scanning wallet";
    }
};

/**
 * @brief Phase W.2.1: Wallet Sync Status Aggregator
 *
 * Aggregates sync state from multiple components into single WalletSyncStatus.
 * Pure aggregation - no side effects, no state mutation.
 */
class WalletSyncStatusAggregator {
public:
    /**
     * @brief Create sync status from current system state
     *
     * Aggregates from:
     * - ChainDB (chain height)
     * - BlockDownloadScheduler (sync phase, block progress)
     * - HeaderSyncManager (header progress) [optional]
     * - Wallet (scan height) [optional]
     *
     * @param chain_db ChainDB instance (required)
     * @param scheduler BlockDownloadScheduler (optional, for phase detection)
     * @param header_sync HeaderSyncManager (optional, for header progress)
     * @param wallet Wallet (optional, for scan progress)
     * @return WalletSyncStatus Current sync state
     */
    static WalletSyncStatus CreateFromComponents(
        const ChainDB* chain_db,
        const BlockDownloadScheduler* scheduler = nullptr,
        const HeaderSyncManager* header_sync = nullptr,
        const Wallet* wallet = nullptr
    );

    /**
     * @brief Calculate phase-aware overall progress
     *
     * Phase-specific weighting:
     * - IBD: 40% headers, 40% blocks, 20% scan
     * - CATCHING_UP: 20% headers, 60% blocks, 20% scan
     * - STEADY_STATE: 10% headers, 10% blocks, 80% scan
     *
     * @param status Sync status with component progress filled
     * @return Overall progress [0.0, 1.0]
     */
    static double CalculateOverallProgress(const WalletSyncStatus& status);

private:
    // No instances - static utility only
    WalletSyncStatusAggregator() = delete;
};

} // namespace dinero
