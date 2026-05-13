#include "wallet/wallet_sync_status.h"
#include "storage/chain_db.h"
#include "p2p/block_download_scheduler.h"
#include "consensus/header_sync_manager.h"  // Phase D: Header sync integration
#include "common/logger.h"

namespace dinero {

// ============================================================================
// WalletSyncStatusAggregator Implementation
// ============================================================================

WalletSyncStatus WalletSyncStatusAggregator::CreateFromComponents(
    const ChainDB* chain_db,
    const BlockDownloadScheduler* scheduler,
    const HeaderSyncManager* header_sync,
    const Wallet* wallet
) {
    WalletSyncStatus status;

    // ========================================================================
    // 1. Chain Height (required, from ChainDB)
    // ========================================================================
    if (chain_db) {
        auto tip = chain_db->getTip();
        if (tip) {
            status.chain_height = tip->height;
            status.blocks_synced = tip->height;  // Assume synced to tip
        } else {
            dinero::g_logger.warning("WalletSyncStatus: ChainDB has no tip");
            status.chain_height = 0;
            status.blocks_synced = 0;
        }
    } else {
        dinero::g_logger.warning("WalletSyncStatus: ChainDB not provided");
        status.chain_height = 0;
        status.blocks_synced = 0;
    }

    // ========================================================================
    // 2. Sync Phase (from BlockDownloadScheduler via G.12)
    // ========================================================================
    if (scheduler) {
        status.phase = scheduler->getSyncPhase();
    } else {
        status.phase = SyncPhase::STEADY_STATE;
        dinero::g_logger.debug("WalletSyncStatus: No scheduler, defaulting to STEADY_STATE");
    }

    // ========================================================================
    // 3. Header Progress (optional, from HeaderSyncManager)
    // ========================================================================
    if (header_sync) {
        auto header_status = header_sync->GetStatus();
        status.headers_synced = header_status.headers_synced;
        status.headers_total = header_status.headers_target;

        // If target is 0, fall back to chain height
        if (status.headers_total == 0) {
            status.headers_total = status.chain_height;
        }
    } else {
        // No header sync data - assume complete (use blocks as proxy)
        status.headers_synced = status.blocks_synced;
        status.headers_total = status.chain_height;
    }

    // ========================================================================
    // 4. Block Progress (from scheduler or chain_db)
    // ========================================================================
    status.blocks_total = status.chain_height;

    if (scheduler) {
        // TODO: Get actual sync target from scheduler (best known height)
        // For now, use chain_height
        dinero::g_logger.debug("WalletSyncStatus: BlockDownloadScheduler integration pending");
    }

    // ========================================================================
    // 5. Wallet Scan Progress (optional, from Wallet)
    // ========================================================================
    if (wallet) {
        // TODO: Wallet scan height integration (Phase A)
        // For now, assume wallet is synced to chain tip
        status.wallet_scan_height = status.chain_height;

        dinero::g_logger.debug("WalletSyncStatus: Wallet scan integration pending, "
                              "assuming synced to tip");
    } else {
        // No wallet - assume scan complete (or not applicable)
        status.wallet_scan_height = status.chain_height;
    }

    // ========================================================================
    // 6. Reorg State (from ChainDB or scheduler)
    // ========================================================================
    // TODO: Reorg detection (Phase W.2.4)
    status.is_reorg_in_progress = false;
    status.last_reorg_depth = 0;

    // ========================================================================
    // 7. Calculate Overall Progress
    // ========================================================================
    status.overall_progress = CalculateOverallProgress(status);

    // ========================================================================
    // 8. ETA Estimation (Phase W.2.2 - deferred)
    // ========================================================================
    status.eta = std::nullopt;  // Not implemented yet

    // ========================================================================
    // 9. Validate & Return
    // ========================================================================
    if (!status.IsValid()) {
        dinero::g_logger.error("WalletSyncStatus: Invalid state detected! "
                              "headers=" + std::to_string(status.headers_synced) + "/" +
                              std::to_string(status.headers_total) +
                              ", blocks=" + std::to_string(status.blocks_synced) + "/" +
                              std::to_string(status.blocks_total) +
                              ", scan=" + std::to_string(status.wallet_scan_height) + "/" +
                              std::to_string(status.chain_height) +
                              ", progress=" + std::to_string(status.overall_progress));
    }

    dinero::g_logger.info("WalletSyncStatus: " + status.GetStatusDescription() +
                         " (progress=" + std::to_string(static_cast<int>(status.overall_progress * 100)) + "%)");

    return status;
}

double WalletSyncStatusAggregator::CalculateOverallProgress(const WalletSyncStatus& status) {
    // Get component progress values
    double headers_prog = status.headers_progress();
    double blocks_prog = status.blocks_progress();
    double scan_prog = status.wallet_scan_progress();

    // Phase-specific weighting
    double overall = 0.0;

    switch (status.phase) {
        case SyncPhase::IBD:
            // Initial Block Download: headers and blocks equally important
            overall = 0.4 * headers_prog + 0.4 * blocks_prog + 0.2 * scan_prog;
            break;

        case SyncPhase::CATCHING_UP:
            // Catching up: blocks dominant (headers likely complete)
            overall = 0.2 * headers_prog + 0.6 * blocks_prog + 0.2 * scan_prog;
            break;

        case SyncPhase::STEADY_STATE:
            // Steady state: wallet scan dominant (headers/blocks complete)
            overall = 0.1 * headers_prog + 0.1 * blocks_prog + 0.8 * scan_prog;
            break;

        default:
            // Unknown phase - equal weighting
            overall = (headers_prog + blocks_prog + scan_prog) / 3.0;
            break;
    }

    // Clamp to [0.0, 1.0]
    if (overall < 0.0) overall = 0.0;
    if (overall > 1.0) overall = 1.0;

    // Never show 100% unless everything is actually complete
    if (overall >= 0.9999) {
        bool all_complete = (headers_prog >= 0.9999 &&
                            blocks_prog >= 0.9999 &&
                            scan_prog >= 0.9999);
        if (!all_complete) {
            overall = 0.999;  // Cap at 99.9%
        }
    }

    return overall;
}

} // namespace dinero
