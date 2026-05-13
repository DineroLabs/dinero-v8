#include "wallet/slow_reason_analyzer.h"
#include "storage/chain_db.h"
#include "daemon/mempool.h"  // Phase A: Mempool stats for pressure detection
#include "p2p/peer_manager.h"  // Phase W.2.6 Enhancement #4: PeerManager quality metrics
#include "common/logger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dinero {

// ============================================================================
// Constructor
// ============================================================================

SlowReasonAnalyzer::SlowReasonAnalyzer(const Thresholds& thresholds)
    : thresholds_(thresholds)
{
}

// ============================================================================
// Main Analysis
// ============================================================================

SlowReasonInfo SlowReasonAnalyzer::Analyze(
    const WalletSyncStatus& status,
    const ChainDB* chain_db,
    const Mempool* mempool,
    const PeerManager* peer_manager,
    const ReorgDetector* reorg_detector,
    uint64_t current_time_ms
) {
    // Clear previous results
    detected_reasons_.clear();

    // Run all checks
    auto network_ibd = CheckNetworkIBD(status, peer_manager);
    auto low_peer_quality = CheckLowPeerQuality(peer_manager);
    auto disk_bound = CheckDiskBound(chain_db);
    auto mempool_pressure = CheckHighMempoolPressure(mempool);
    auto reorg_recovery = CheckReorgRecovery(reorg_detector, current_time_ms);
    auto wallet_rescan = CheckWalletRescan(status);

    // Collect detected reasons
    if (reorg_recovery.has_value()) {
        detected_reasons_.push_back(reorg_recovery.value());
    }
    if (disk_bound.has_value()) {
        detected_reasons_.push_back(disk_bound.value());
    }
    if (low_peer_quality.has_value()) {
        detected_reasons_.push_back(low_peer_quality.value());
    }
    if (mempool_pressure.has_value()) {
        detected_reasons_.push_back(mempool_pressure.value());
    }
    if (wallet_rescan.has_value()) {
        detected_reasons_.push_back(wallet_rescan.value());
    }
    if (network_ibd.has_value()) {
        detected_reasons_.push_back(network_ibd.value());
    }

    // Sort by impact (highest first)
    std::sort(detected_reasons_.begin(), detected_reasons_.end(),
              [](const SlowReasonInfo& a, const SlowReasonInfo& b) {
                  return a.impact_factor > b.impact_factor;
              });

    // Return most impactful reason, or NONE
    if (!detected_reasons_.empty()) {
        dinero::g_logger.debug("SlowReasonAnalyzer: Detected " +
                              std::to_string(detected_reasons_.size()) +
                              " slow reasons, primary: " +
                              detected_reasons_[0].description);
        return detected_reasons_[0];
    }

    // No slowness detected
    SlowReasonInfo none;
    none.reason = SlowReason::NONE;
    none.severity = SlowSeverity::NONE;
    none.description = GetSlowReasonDescription(SlowReason::NONE);
    none.suggestion = GetSlowReasonSuggestion(SlowReason::NONE);
    none.impact_factor = 0.0;

    return none;
}

// ============================================================================
// Individual Checks
// ============================================================================

std::optional<SlowReasonInfo> SlowReasonAnalyzer::CheckNetworkIBD(
    const WalletSyncStatus& status,
    const PeerManager* peer_manager
) {
    // Network IBD if we're in IBD phase and far from tip
    if (status.phase != SyncPhase::IBD) {
        return std::nullopt;
    }

    // If we're less than 95% synced, assume network IBD
    if (status.overall_progress < 0.95) {
        SlowReasonInfo info;
        info.reason = SlowReason::NETWORK_IBD;
        info.severity = GetSlowReasonSeverity(SlowReason::NETWORK_IBD);
        info.description = GetSlowReasonDescription(SlowReason::NETWORK_IBD);
        info.suggestion = GetSlowReasonSuggestion(SlowReason::NETWORK_IBD);

        // Impact: Low (this is expected during IBD)
        info.impact_factor = 0.2;

        // Context
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "Overall progress: " << (status.overall_progress * 100.0) << "%";
        info.context.push_back(oss.str());

        // TODO: Add peer manager integration for more details
        // - Count of peers at similar heights
        // - Network hash rate
        if (peer_manager) {
            info.context.push_back("Peer metrics: (integration pending)");
        }

        return info;
    }

    return std::nullopt;
}

std::optional<SlowReasonInfo> SlowReasonAnalyzer::CheckLowPeerQuality(
    const PeerManager* peer_manager
) {
    if (!peer_manager) {
        return std::nullopt;
    }

    // Phase W.2.6 Enhancement #4: Real peer quality detection
    auto stats = peer_manager->GetQualityStats();

    // No peers connected - can't assess quality
    if (stats.total_peers == 0) {
        return std::nullopt;
    }

    // Check for poor peer quality conditions
    bool high_latency = stats.avg_ping_ms > thresholds_.max_peer_ping_ms;
    bool low_bandwidth = stats.avg_download_kbps < thresholds_.min_download_rate_kbps
                         && stats.avg_download_kbps > 0.0;  // Only if tracking bandwidth
    bool mostly_bad_peers = stats.bad_peers > stats.good_peers;

    if (high_latency || low_bandwidth || mostly_bad_peers) {
        SlowReasonInfo info;
        info.reason = SlowReason::LOW_PEER_QUALITY;
        info.severity = SlowSeverity::MODERATE;
        info.impact_factor = 0.4;  // Peer quality has moderate impact on sync speed

        info.description = "Poor peer quality detected (high latency or low bandwidth)";
        info.suggestion = "Try adding better peers or checking your network connection.";

        // Add diagnostic context
        info.context.push_back(
            "Avg ping: " + std::to_string(static_cast<int>(stats.avg_ping_ms)) + " ms"
        );

        if (stats.avg_download_kbps > 0.0) {
            info.context.push_back(
                "Avg download: " + std::to_string(static_cast<int>(stats.avg_download_kbps)) + " KB/s"
            );
        }

        info.context.push_back(
            "Good peers: " + std::to_string(stats.good_peers) +
            " / " + std::to_string(stats.total_peers)
        );

        return info;
    }

    return std::nullopt;
}

std::optional<SlowReasonInfo> SlowReasonAnalyzer::CheckDiskBound(
    const ChainDB* chain_db
) {
    // TODO: ChainDB disk metrics integration
    // For now, we can't detect this without disk metrics
    if (!chain_db) {
        return std::nullopt;
    }

    // Stub: Would check:
    // - Block validation rate (blocks/sec)
    // - RocksDB write stalls
    // - Disk queue depth

    return std::nullopt;
}

std::optional<SlowReasonInfo> SlowReasonAnalyzer::CheckHighMempoolPressure(
    const Mempool* mempool
) {
    if (!mempool) {
        return std::nullopt;
    }

    // Get mempool statistics
    auto stats = mempool->getStats();

    // Convert threshold from MB to bytes
    uint64_t threshold_bytes = thresholds_.high_mempool_size_mb * 1024 * 1024;

    // Check if mempool size exceeds threshold
    if (stats.total_size < threshold_bytes) {
        return std::nullopt;
    }

    SlowReasonInfo info;
    info.reason = SlowReason::HIGH_MEMPOOL_PRESSURE;
    info.severity = SlowSeverity::MODERATE;
    info.impact_factor = 0.3;  // Mempool pressure has moderate impact

    info.description = "High mempool pressure detected (may delay transaction propagation)";
    info.suggestion = "Consider using a higher fee rate for faster confirmation.";

    // Add diagnostic context
    double size_mb = static_cast<double>(stats.total_size) / (1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "Mempool size: " << size_mb << " MB";
    info.context.push_back(oss.str());

    info.context.push_back("Transactions: " + std::to_string(stats.tx_count));

    if (stats.avg_fee_rate > 0) {
        oss.str("");
        oss << "Avg fee rate: " << std::fixed << std::setprecision(1) << stats.avg_fee_rate << " sat/vB";
        info.context.push_back(oss.str());
    }

    return info;
}

std::optional<SlowReasonInfo> SlowReasonAnalyzer::CheckReorgRecovery(
    const ReorgDetector* reorg_detector,
    uint64_t current_time_ms
) {
    if (!reorg_detector) {
        return std::nullopt;
    }

    // Check if reorg is currently in progress
    if (reorg_detector->IsReorgInProgress()) {
        SlowReasonInfo info;
        info.reason = SlowReason::REORG_RECOVERY;
        info.severity = GetSlowReasonSeverity(SlowReason::REORG_RECOVERY);
        info.description = GetSlowReasonDescription(SlowReason::REORG_RECOVERY);
        info.suggestion = GetSlowReasonSuggestion(SlowReason::REORG_RECOVERY);

        // Impact: High during reorg (ETA is frozen)
        info.impact_factor = 0.8;

        // Context
        auto current_reorg = reorg_detector->GetCurrentReorg();
        if (current_reorg.has_value()) {
            std::ostringstream oss;
            oss << "Reorg depth: " << current_reorg->depth << " blocks";
            info.context.push_back(oss.str());

            if (current_reorg->affected_tx_count > 0) {
                oss.str("");
                oss << "Affected transactions: " << current_reorg->affected_tx_count;
                info.context.push_back(oss.str());
            }
        }

        return info;
    }

    // Check if we recently finished a reorg (within recovery window)
    auto recent_reorgs = reorg_detector->GetRecentReorgs(1);
    if (!recent_reorgs.empty()) {
        const auto& last_reorg = recent_reorgs[0];

        // Check if within recovery window
        uint64_t time_since_reorg = current_time_ms - last_reorg.timestamp_ms;
        if (time_since_reorg < thresholds_.reorg_recovery_window_ms) {
            SlowReasonInfo info;
            info.reason = SlowReason::REORG_RECOVERY;
            info.severity = GetSlowReasonSeverity(SlowReason::REORG_RECOVERY);
            info.description = "Recovering from recent chain reorganization";
            info.suggestion = GetSlowReasonSuggestion(SlowReason::REORG_RECOVERY);

            // Impact: Moderate (recovery in progress)
            info.impact_factor = 0.5;

            // Context
            std::ostringstream oss;
            uint64_t seconds_ago = time_since_reorg / 1000;
            oss << "Reorg completed " << seconds_ago << " seconds ago (depth: "
                << last_reorg.depth << ")";
            info.context.push_back(oss.str());

            return info;
        }
    }

    return std::nullopt;
}

std::optional<SlowReasonInfo> SlowReasonAnalyzer::CheckWalletRescan(
    const WalletSyncStatus& status
) {
    // Wallet rescan if:
    // 1. We're in STEADY_STATE (blocks complete)
    // 2. Wallet scan is significantly behind chain tip

    if (status.phase != SyncPhase::STEADY_STATE) {
        return std::nullopt;
    }

    // Check if wallet scan is lagging
    uint64_t scan_lag = 0;
    if (status.chain_height > status.wallet_scan_height) {
        scan_lag = status.chain_height - status.wallet_scan_height;
    }

    if (scan_lag >= thresholds_.wallet_scan_lag_blocks) {
        SlowReasonInfo info;
        info.reason = SlowReason::WALLET_RESCAN;
        info.severity = GetSlowReasonSeverity(SlowReason::WALLET_RESCAN);
        info.description = GetSlowReasonDescription(SlowReason::WALLET_RESCAN);
        info.suggestion = GetSlowReasonSuggestion(SlowReason::WALLET_RESCAN);

        // Impact: Low to Moderate (depends on lag)
        // 0.3 for 1000 blocks, 0.6 for 10000+ blocks
        double lag_ratio = static_cast<double>(scan_lag) / 10000.0;
        info.impact_factor = std::min(0.6, 0.3 + lag_ratio * 0.3);

        // Context
        std::ostringstream oss;
        oss << "Wallet scan: " << status.wallet_scan_height << " / "
            << status.chain_height << " (" << scan_lag << " blocks behind)";
        info.context.push_back(oss.str());

        double scan_progress = status.wallet_scan_progress();
        oss.str("");
        oss << std::fixed << std::setprecision(1);
        oss << "Scan progress: " << (scan_progress * 100.0) << "%";
        info.context.push_back(oss.str());

        return info;
    }

    return std::nullopt;
}

// ============================================================================
// Impact Calculation
// ============================================================================

double SlowReasonAnalyzer::CalculateImpact(const SlowReasonInfo& info) const {
    // Impact already calculated in individual checks
    return info.impact_factor;
}

} // namespace dinero
