// SPDX-License-Identifier: MIT
// Dinero - Wallet Sync UX RPC Handler Implementations (Phase W.2.6)

#include "rpc/wallet_sync_rpc_handlers.h"
#include "rpc/wallet_sync_aggregator.h"
#include "rpc/rpc_registry.h"
#include "din_json.h"
#include "wallet/wallet_sync_status.h"
#include "wallet/reorg_detector.h"
#include "wallet/slow_reason_analyzer.h"
#include "wallet/slow_reason.h"
#include "wallet/sync_progress_tracker.h"
#include "daemon/execution_context.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/block_download_scheduler.h"
#include "common/logger.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace dinero;

extern RpcRegistry g_rpcRegistry;

// ============================================================================
// Global State Management for W.2 Components
// ============================================================================

namespace {

/**
 * @brief Get global ReorgDetector instance (singleton)
 */
ReorgDetector& GetReorgDetector() {
    static ReorgDetector instance;
    return instance;
}

/**
 * @brief Get global SlowReasonAnalyzer instance (singleton)
 */
SlowReasonAnalyzer& GetSlowReasonAnalyzer() {
    static SlowReasonAnalyzer instance;
    return instance;
}

/**
 * @brief Get global SyncProgressTracker instance (singleton)
 */
SyncProgressTracker& GetSyncProgressTracker() {
    static SyncProgressTracker instance;
    return instance;
}

} // anonymous namespace

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

/**
 * @brief Convert SyncPhase enum to string
 */
std::string syncPhaseToString(SyncPhase phase) {
    switch (phase) {
        case SyncPhase::IBD:
            return "ibd";
        case SyncPhase::CATCHING_UP:
            return "catching_up";
        case SyncPhase::STEADY_STATE:
            return "steady_state";
        default:
            return "unknown";
    }
}

/**
 * @brief Convert SlowReason enum to string
 */
std::string slowReasonToString(SlowReason reason) {
    switch (reason) {
        case SlowReason::NETWORK_IBD:
            return "network_ibd";
        case SlowReason::LOW_PEER_QUALITY:
            return "low_peer_quality";
        case SlowReason::DISK_BOUND:
            return "disk_bound";
        case SlowReason::HIGH_MEMPOOL_PRESSURE:
            return "high_mempool_pressure";
        case SlowReason::REORG_RECOVERY:
            return "reorg_recovery";
        case SlowReason::WALLET_RESCAN:
            return "wallet_rescan";
        case SlowReason::NONE:
            return "none";
        default:
            return "unknown";
    }
}

/**
 * @brief Convert SlowSeverity enum to string
 */
std::string slowSeverityToString(SlowSeverity severity) {
    switch (severity) {
        case SlowSeverity::NONE:
            return "none";
        case SlowSeverity::LOW:
            return "low";
        case SlowSeverity::MODERATE:
            return "moderate";
        case SlowSeverity::HIGH:
            return "high";
        default:
            return "unknown";
    }
}

/**
 * @brief Format seconds as human-readable duration
 */
std::string formatDuration(int64_t seconds) {
    if (seconds < 60) {
        return std::to_string(seconds) + "s";
    } else if (seconds < 3600) {
        int minutes = seconds / 60;
        int secs = seconds % 60;
        return std::to_string(minutes) + "m " + std::to_string(secs) + "s";
    } else if (seconds < 86400) {
        int hours = seconds / 3600;
        int minutes = (seconds % 3600) / 60;
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    } else {
        int days = seconds / 86400;
        int hours = (seconds % 86400) / 3600;
        return std::to_string(days) + "d " + std::to_string(hours) + "h";
    }
}

} // anonymous namespace

// ============================================================================
// RPC Handler: getsyncstatus
// ============================================================================

/**
 * @brief Get complete wallet sync status
 *
 * Returns:
 * {
 *   "phase": "ibd|catching_up|steady_state",
 *   "phase_name": "Initial Block Download",
 *   "overall_progress": 0.0-1.0,
 *   "overall_progress_percent": "50.5%",
 *   "eta": 1234 (seconds),
 *   "eta_formatted": "20m 34s",
 *   "headers": {
 *     "synced": 5000,
 *     "total": 10000,
 *     "progress": 0.5
 *   },
 *   "blocks": {
 *     "synced": 4500,
 *     "total": 10000,
 *     "progress": 0.45
 *   },
 *   "wallet_scan": {
 *     "height": 4000,
 *     "chain_height": 10000,
 *     "progress": 0.4
 *   },
 *   "reorg": {
 *     "in_progress": false,
 *     "last_depth": 0
 *   },
 *   "slow_reason": {
 *     "reason": "network_ibd",
 *     "description": "Initial blockchain download - network is syncing",
 *     "suggestion": "This is normal during initial sync. Please wait."
 *   },
 *   "status_description": "IBD: Syncing headers and blocks (50.5%)",
 *   "is_synced": false
 * }
 */
din::Json rpc_getsyncstatus(const ::ExecutionContext& ctx, const din::Json& params) {
    try {
        // ====================================================================
        // Step 1: Build base status from live components (single aggregation point)
        // ====================================================================
        WalletSyncStatus status = BuildWalletSyncStatusFromContext(ctx);

        // Get current timestamp
        uint64_t current_time_ms = GetCurrentTimeMs();

        // ====================================================================
        // Step 2: Update reorg state (Phase W.2.4)
        // ====================================================================
        auto& reorg_detector = GetReorgDetector();
        reorg_detector.UpdateSyncStatus(status);

        // ====================================================================
        // Step 3: Calculate ETA (Phase W.2.2)
        // ====================================================================
        auto& sync_tracker = GetSyncProgressTracker();
        sync_tracker.Update(status, current_time_ms);
        status.eta = sync_tracker.CalculateETA(status, current_time_ms);

        // ====================================================================
        // Step 4: Analyze slow reasons (Phase W.2.5)
        // ====================================================================
        const ChainDB* chain_db = nullptr;
        if (ctx.daemon && ctx.daemon->chainstate) {
            chain_db = ctx.daemon->chainstate->GetChainDB();
        }

        auto& slow_analyzer = GetSlowReasonAnalyzer();
        auto slow_info = slow_analyzer.Analyze(
            status,
            chain_db,
            ctx.mempool_v2,  // Direct access in global ExecutionContext
            nullptr,  // PeerManager not in ExecutionContext yet
            &reorg_detector,
            current_time_ms
        );

        status.slow_reason_description = slow_info.description;
        status.slow_reason_suggestion = slow_info.suggestion;

        // Build JSON response
        din::Json result;

        // Phase information
        result["phase"] = syncPhaseToString(status.phase);
        result["phase_name"] = status.GetPhaseName();

        // Overall progress
        result["overall_progress"] = status.overall_progress;
        std::ostringstream progress_str;
        progress_str << std::fixed << std::setprecision(1)
                     << (status.overall_progress * 100.0) << "%";
        result["overall_progress_percent"] = progress_str.str();

        // ETA
        if (status.eta.has_value()) {
            int64_t eta_seconds = status.eta.value().count();
            result["eta"] = eta_seconds;
            result["eta_formatted"] = formatDuration(eta_seconds);
        } else {
            result["eta"] = Json::Value::null;
            result["eta_formatted"] = "Estimating...";
        }

        // Headers progress
        din::Json headers;
        headers["synced"] = static_cast<uint64_t>(status.headers_synced);
        headers["total"] = static_cast<uint64_t>(status.headers_total);
        headers["progress"] = status.headers_progress();
        result["headers"] = headers;

        // Blocks progress
        din::Json blocks;
        blocks["synced"] = static_cast<uint64_t>(status.blocks_synced);
        blocks["total"] = static_cast<uint64_t>(status.blocks_total);
        blocks["progress"] = status.blocks_progress();
        result["blocks"] = blocks;

        // Block download scheduler telemetry
        if (ctx.daemon && ctx.daemon->block_download) {
            const size_t in_flight = ctx.daemon->block_download->GetInFlightCount();
            const size_t queued = ctx.daemon->block_download->GetQueuedBlockCount();
            const size_t queued_not_in_flight = (queued > in_flight) ? (queued - in_flight) : 0;

            din::Json scheduler;
            scheduler["in_flight"] = static_cast<uint64_t>(in_flight);
            scheduler["queued"] = static_cast<uint64_t>(queued);
            scheduler["queued_not_in_flight"] = static_cast<uint64_t>(queued_not_in_flight);
            scheduler["missing"] = static_cast<uint64_t>(ctx.daemon->block_download->GetMissingBlockCount());
            scheduler["is_fully_synchronized"] = ctx.daemon->block_download->IsFullySynchronized();
            result["scheduler"] = scheduler;
        }

        // Wallet scan progress
        din::Json wallet_scan;
        wallet_scan["height"] = static_cast<uint64_t>(status.wallet_scan_height);
        wallet_scan["chain_height"] = static_cast<uint64_t>(status.chain_height);
        wallet_scan["progress"] = status.wallet_scan_progress();
        result["wallet_scan"] = wallet_scan;

        // Reorg state
        din::Json reorg;
        reorg["in_progress"] = status.is_reorg_in_progress;
        reorg["last_depth"] = status.last_reorg_depth;
        result["reorg"] = reorg;

        // Slow reason
        din::Json slow_reason;
        if (!status.slow_reason_description.empty()) {
            // Parse reason from description (simplified)
            slow_reason["description"] = status.slow_reason_description;
            slow_reason["suggestion"] = status.slow_reason_suggestion;
        } else {
            slow_reason["description"] = "Syncing normally";
            slow_reason["suggestion"] = "";
        }
        result["slow_reason"] = slow_reason;

        // Status description
        result["status_description"] = status.GetStatusDescription();

        // Fully synced flag
        result["is_synced"] = status.IsFullySynced();

        std::string scheduler_metrics = "scheduler=unavailable";
        if (ctx.daemon && ctx.daemon->block_download) {
            const size_t in_flight = ctx.daemon->block_download->GetInFlightCount();
            const size_t queued = ctx.daemon->block_download->GetQueuedBlockCount();
            const size_t queued_not_in_flight = (queued > in_flight) ? (queued - in_flight) : 0;
            scheduler_metrics = "scheduler(in_flight=" + std::to_string(in_flight) +
                                ", queued=" + std::to_string(queued) +
                                ", queued_not_in_flight=" + std::to_string(queued_not_in_flight) + ")";
        }

        dinero::g_logger.debug("RPC getsyncstatus: phase=" + syncPhaseToString(status.phase) +
                              ", progress=" + std::to_string(status.overall_progress) +
                              ", " + scheduler_metrics);

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

// ============================================================================
// RPC Handler: getreorginfo
// ============================================================================

/**
 * @brief Get recent chain reorganization information
 *
 * Parameters:
 * - count: Number of recent reorgs to return (default: 5, max: 20)
 *
 * Returns:
 * {
 *   "current_reorg": {
 *     "in_progress": true,
 *     "depth": 3,
 *     "detected_at_height": 10000,
 *     "balance_change": -50000000,
 *     "affected_tx_count": 2,
 *     "severity": "moderate",
 *     "description": "Reorganization detected (depth: 3, moderate) - 2 transactions affected"
 *   } OR null,
 *   "recent_reorgs": [
 *     {
 *       "depth": 3,
 *       "detected_at_height": 10000,
 *       "timestamp": 1234567890000,
 *       "balance_change": -50000000,
 *       "affected_tx_count": 2,
 *       "severity": "moderate",
 *       "description": "...",
 *       "requires_alert": true
 *     }
 *   ],
 *   "total_reorgs": 1,
 *   "max_depth": 3
 * }
 */
din::Json rpc_getreorginfo(const ::ExecutionContext& ctx, const din::Json& params) {
    try {
        // Get count parameter (positional)
        int count = 5;
        if (params.size() > 0) {
            count = params[0].as<int>();
            if (count < 1) count = 1;
            if (count > 20) count = 20;
        }

        // Get ReorgDetector instance
        auto& reorg_detector = GetReorgDetector();

        din::Json result;

        // Current reorg (if any)
        auto current_reorg = reorg_detector.GetCurrentReorg();
        if (current_reorg.has_value() && current_reorg->is_in_progress) {
            din::Json reorg_json;
            reorg_json["in_progress"] = current_reorg->is_in_progress;
            reorg_json["depth"] = current_reorg->depth;
            reorg_json["detected_at_height"] = static_cast<uint64_t>(current_reorg->detected_at_height);
            reorg_json["balance_change"] = current_reorg->balance_change;
            reorg_json["affected_tx_count"] = current_reorg->affected_tx_count;
            reorg_json["severity"] = current_reorg->GetSeverity();
            reorg_json["description"] = current_reorg->GetDescription();
            result["current_reorg"] = reorg_json;
        } else {
            result["current_reorg"] = Json::Value::null;
        }

        // Recent reorgs
        auto recent = reorg_detector.GetRecentReorgs(count);
        din::Json recent_reorgs(Json::arrayValue);

        int max_depth = 0;
        for (const auto& reorg : recent) {
            din::Json reorg_json;
            reorg_json["depth"] = reorg.depth;
            reorg_json["detected_at_height"] = static_cast<uint64_t>(reorg.detected_at_height);
            reorg_json["timestamp"] = static_cast<uint64_t>(reorg.timestamp_ms);
            reorg_json["balance_change"] = reorg.balance_change;
            reorg_json["affected_tx_count"] = reorg.affected_tx_count;
            reorg_json["severity"] = reorg.GetSeverity();
            reorg_json["description"] = reorg.GetDescription();
            reorg_json["requires_alert"] = reorg.RequiresUserAlert();
            recent_reorgs.append(reorg_json);

            if (reorg.depth > max_depth) {
                max_depth = reorg.depth;
            }
        }

        result["recent_reorgs"] = recent_reorgs;
        result["total_reorgs"] = static_cast<int>(recent.size());
        result["max_depth"] = max_depth;

        dinero::g_logger.debug("RPC getreorginfo: count=" + std::to_string(count) +
                              ", total=" + std::to_string(recent.size()));

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

// ============================================================================
// RPC Handler: getslowreason
// ============================================================================

/**
 * @brief Get detailed analysis of why sync is slow
 *
 * Returns:
 * {
 *   "primary_reason": {
 *     "reason": "network_ibd",
 *     "severity": "none",
 *     "description": "Initial blockchain download - network is syncing",
 *     "suggestion": "This is normal during initial sync. Please wait.",
 *     "impact_factor": 0.2,
 *     "context": [
 *       "Overall progress: 50.5%"
 *     ]
 *   },
 *   "all_reasons": [
 *     {
 *       "reason": "network_ibd",
 *       "severity": "none",
 *       "description": "...",
 *       "suggestion": "...",
 *       "impact_factor": 0.2
 *     }
 *   ],
 *   "is_slow": false,
 *   "recommendation": "This is normal during initial sync. Please wait."
 * }
 */
din::Json rpc_getslowreason(const ::ExecutionContext& ctx, const din::Json& params) {
    try {
        // Build current WalletSyncStatus (similar to getsyncstatus)
        WalletSyncStatus status = BuildWalletSyncStatusFromContext(ctx);

        // Get current timestamp
        uint64_t current_time_ms = GetCurrentTimeMs();

        // Get analyzers
        auto& reorg_detector = GetReorgDetector();
        auto& slow_analyzer = GetSlowReasonAnalyzer();
        const ChainDB* chain_db = nullptr;
        if (ctx.daemon && ctx.daemon->chainstate) {
            chain_db = ctx.daemon->chainstate->GetChainDB();
        }

        // Analyze current state
        auto primary_reason = slow_analyzer.Analyze(
            status,
            chain_db,
            ctx.mempool_v2,  // Direct access in global ExecutionContext
            nullptr,  // PeerManager
            &reorg_detector,
            current_time_ms
        );

        din::Json result;

        // Primary reason
        din::Json primary;
        primary["reason"] = slowReasonToString(primary_reason.reason);
        primary["severity"] = slowSeverityToString(primary_reason.severity);
        primary["description"] = primary_reason.description;
        primary["suggestion"] = primary_reason.suggestion;
        primary["impact_factor"] = primary_reason.impact_factor;

        din::Json context(Json::arrayValue);
        for (const auto& ctx_str : primary_reason.context) {
            context.append(ctx_str);
        }
        primary["context"] = context;

        result["primary_reason"] = primary;

        // All reasons (sorted by impact)
        auto all_reasons_vec = slow_analyzer.GetAllReasons();
        din::Json all_reasons(Json::arrayValue);

        for (const auto& reason : all_reasons_vec) {
            din::Json reason_json;
            reason_json["reason"] = slowReasonToString(reason.reason);
            reason_json["severity"] = slowSeverityToString(reason.severity);
            reason_json["description"] = reason.description;
            reason_json["suggestion"] = reason.suggestion;
            reason_json["impact_factor"] = reason.impact_factor;

            din::Json reason_context(Json::arrayValue);
            for (const auto& ctx_str : reason.context) {
                reason_context.append(ctx_str);
            }
            reason_json["context"] = reason_context;

            all_reasons.append(reason_json);
        }

        result["all_reasons"] = all_reasons;

        // Determine if sync is slow (any reason with impact > 0.3)
        bool is_slow = (primary_reason.reason != SlowReason::NONE &&
                       primary_reason.impact_factor > 0.3);
        result["is_slow"] = is_slow;

        // Recommendation is the primary suggestion
        result["recommendation"] = primary_reason.suggestion;

        dinero::g_logger.debug("RPC getslowreason: primary=" +
                              slowReasonToString(primary_reason.reason) +
                              ", impact=" + std::to_string(primary_reason.impact_factor));

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

// ============================================================================
// RPC Method Registration
// ============================================================================

namespace dinero {
namespace rpc {

void registerWalletSyncMethods() {
    g_rpcRegistry.registerHandler("wallet.getsyncstatus",
                                 rpc_getsyncstatus,
                                 RegisterMode::Overwrite,
                                 "wallet-sync");

    g_rpcRegistry.registerHandler("wallet.getreorginfo",
                                 rpc_getreorginfo,
                                 RegisterMode::Overwrite,
                                 "wallet-sync");

    g_rpcRegistry.registerHandler("wallet.getslowreason",
                                 rpc_getslowreason,
                                 RegisterMode::Overwrite,
                                 "wallet-sync");

    dinero::g_logger.info("Registered wallet sync RPC methods: wallet.getsyncstatus, wallet.getreorginfo, wallet.getslowreason");
}

} // namespace rpc
} // namespace dinero
