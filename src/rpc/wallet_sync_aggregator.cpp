// SPDX-License-Identifier: MIT
// Dinero - Wallet Sync Status Aggregation (Phase W.2.6)

#include "rpc/wallet_sync_aggregator.h"
#include "wallet/wallet_sync_status.h"
#include "wallet/wallet_manager.h"  // For WalletManager::GetScanStatus()
#include "rpc/rpc_registry.h"  // For global ExecutionContext
#include "daemon/daemon_context.h"  // For DaemonContext
#include "daemon/services/chainstate_service.h"  // For ChainstateService
#include "storage/chain_db.h"  // For ChainDB
#include "consensus/header_sync_manager.h"  // For HeaderSyncManager (Phase W.2.6 Enhancement #2)
#include "consensus/block_download_scheduler.h"  // For BlockDownloadScheduler (Phase W.2.6 Enhancement #3)
#include "common/logger.h"
#include <chrono>

namespace dinero {

/**
 * @brief Build WalletSyncStatus from live execution context
 *
 * Single aggregation point for all sync state.
 * Read-only, no side effects, snapshot semantics only.
 *
 * @param ctx Execution context with component access
 * @return Populated and validated WalletSyncStatus
 * @throws std::runtime_error if critical components unavailable or validation fails
 */
WalletSyncStatus BuildWalletSyncStatusFromContext(const ::ExecutionContext& ctx) {
    WalletSyncStatus status;

    // ========================================================================
    // Critical Component: ChainDB (REQUIRED)
    // ========================================================================
    ChainDB* chain_db = nullptr;

    // Access ChainDB via daemon->chainstate
    if (ctx.daemon && ctx.daemon->chainstate) {
        chain_db = ctx.daemon->chainstate->GetChainDB();
    }

    if (!chain_db) {
        dinero::g_logger.error("BuildWalletSyncStatus: ChainDB unavailable");
        throw std::runtime_error("ChainDB unavailable - cannot determine sync state");
    }

    // Get chain tip (snapshot semantics)
    auto tip_result = chain_db->getTip();
    if (!tip_result.ok()) {
        dinero::g_logger.error("BuildWalletSyncStatus: Failed to get chain tip");
        // Use genesis state (all zeros) if tip not available yet
        status.chain_height = 0;
        status.headers_total = 0;
        status.headers_synced = 0;
        status.blocks_total = 0;
        status.blocks_synced = 0;
    } else {
        const auto& tip = tip_result.value();
        status.chain_height = static_cast<uint64_t>(tip.height);
        status.blocks_total = static_cast<uint64_t>(tip.height);
        status.blocks_synced = static_cast<uint64_t>(tip.height);

        // Phase W.2.6 Enhancement #2: Separate header/block tracking via HeaderSyncManager
        if (dinero::g_header_sync_manager) {
            auto header_status = dinero::g_header_sync_manager->GetStatus();
            status.headers_synced = header_status.headers_synced;
            status.headers_total = header_status.headers_target;

            dinero::g_logger.debug(std::string("BuildWalletSyncStatus: HeaderSyncManager active - ") +
                                  "headers_synced=" + std::to_string(header_status.headers_synced) +
                                  ", headers_target=" + std::to_string(header_status.headers_target) +
                                  ", is_syncing=" + std::to_string(header_status.is_syncing));
        } else {
            // Fallback: headers and blocks are synchronized (legacy behavior)
            status.headers_total = static_cast<uint64_t>(tip.height);
            status.headers_synced = static_cast<uint64_t>(tip.height);
            dinero::g_logger.debug("BuildWalletSyncStatus: HeaderSyncManager not available, using legacy sync");
        }
    }

    // ========================================================================
    // Optional Component: Wallet (for scan height)
    // ========================================================================
    if (ctx.wallet_manager != nullptr) {
        auto* wallet_manager = ctx.wallet_manager;

        // Phase W.2.6 Enhancement #1: True wallet scan height tracking
        auto wallet_scan = wallet_manager->GetScanStatus(status.chain_height);
        status.wallet_scan_height = wallet_scan.scan_height;

        dinero::g_logger.debug("BuildWalletSyncStatus: Wallet scan_height=" +
                              std::to_string(wallet_scan.scan_height) +
                              ", is_scanning=" + std::to_string(wallet_scan.is_scanning));
    } else {
        // No wallet loaded - scan height is null (represented as 0)
        status.wallet_scan_height = 0;
        dinero::g_logger.debug("BuildWalletSyncStatus: No wallet loaded, scan_height=0");
    }

    // ========================================================================
    // Sync Phase Detection
    // ========================================================================
    // Phase W.2.6 Enhancement #3: Get from BlockDownloadScheduler if available
    if (ctx.daemon && ctx.daemon->block_download) {
        status.phase = ctx.daemon->block_download->GetCurrentPhase();
        dinero::g_logger.debug("BuildWalletSyncStatus: Phase from BlockDownloadScheduler: " +
                              std::to_string(static_cast<int>(status.phase)));
    } else {
        // Fallback heuristic (when BlockDownloadScheduler not available)
        if (status.chain_height == 0) {
            status.phase = SyncPhase::STEADY_STATE;
        } else if (status.blocks_synced < status.blocks_total) {
            double progress = static_cast<double>(status.blocks_synced) / status.blocks_total;
            status.phase = (progress < 0.95) ? SyncPhase::IBD : SyncPhase::CATCHING_UP;
        } else {
            status.phase = SyncPhase::STEADY_STATE;
        }
        dinero::g_logger.debug("BuildWalletSyncStatus: Phase from heuristic: " +
                              std::to_string(static_cast<int>(status.phase)));
    }

    // ========================================================================
    // Calculate Overall Progress (Phase W.2.1)
    // ========================================================================
    status.overall_progress = WalletSyncStatusAggregator::CalculateOverallProgress(status);

    // ========================================================================
    // Validate Status (Critical Invariant)
    // ========================================================================
    if (!status.IsValid()) {
        dinero::g_logger.error("BuildWalletSyncStatus: Validation failed - inconsistent state");
        throw std::runtime_error("Invalid WalletSyncStatus: inconsistent component data");
    }

    dinero::g_logger.debug("BuildWalletSyncStatus: phase=" +
                          std::to_string(static_cast<int>(status.phase)) +
                          ", progress=" + std::to_string(status.overall_progress) +
                          ", height=" + std::to_string(status.chain_height));

    return status;
}

/**
 * @brief Get current timestamp in milliseconds
 *
 * @return Milliseconds since epoch
 */
uint64_t GetCurrentTimeMs() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

} // namespace dinero
