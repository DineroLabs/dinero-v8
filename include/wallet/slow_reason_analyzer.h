#pragma once

#include "wallet/slow_reason.h"
#include "wallet/wallet_sync_status.h"
#include "wallet/reorg_detector.h"
#include <memory>
#include <chrono>

// Forward declaration (global namespace)
class PeerManager;

namespace dinero {

// Forward declarations
class ChainDB;
class Mempool;

/**
 * @brief Phase W.2.5: Slow Reason Analyzer
 *
 * Detects and explains why wallet sync is slow.
 *
 * Design Principles:
 * - Evidence-based: Only report what we can measure
 * - User-actionable: Give suggestions when possible
 * - Priority-aware: Report most impactful reason first
 * - Context-rich: Provide supporting data for diagnosis
 *
 * Integration Points:
 * - PeerManager: Peer quality metrics (bandwidth, latency, disconnects)
 * - Mempool: Size, evictions, validation times
 * - ChainDB: Disk I/O metrics, write stalls
 * - ReorgDetector: Recent reorganizations
 * - WalletSyncStatus: Scan progress, sync phase
 */
class SlowReasonAnalyzer {
public:
    /**
     * @brief Thresholds for slow reason detection
     */
    struct Thresholds {
        // Peer quality
        double min_download_rate_kbps;       // < 100 KB/s is slow
        uint64_t max_peer_ping_ms;           // > 500ms is high latency
        double min_peer_availability;        // < 80% uptime is unreliable

        // Disk I/O
        double min_block_validation_rate;    // < 10 blocks/sec is slow
        uint64_t max_disk_write_latency_ms;  // > 100ms is slow

        // Mempool
        uint64_t high_mempool_size_mb;       // > 50 MB is high
        double high_validation_time_ms;      // > 50ms per tx is slow

        // Wallet scan
        uint64_t wallet_scan_lag_blocks;     // > 1000 blocks behind is significant
        double min_wallet_scan_rate;         // < 100 blocks/sec is slow

        // Reorg recovery
        uint64_t reorg_recovery_window_ms;   // 5 minutes

        // Constructor with defaults
        Thresholds()
            : min_download_rate_kbps(100.0)
            , max_peer_ping_ms(500)
            , min_peer_availability(0.8)
            , min_block_validation_rate(10.0)
            , max_disk_write_latency_ms(100)
            , high_mempool_size_mb(50)
            , high_validation_time_ms(50.0)
            , wallet_scan_lag_blocks(1000)
            , min_wallet_scan_rate(100.0)
            , reorg_recovery_window_ms(300000)
        {}
    };

    /**
     * @brief Constructor
     *
     * @param thresholds Detection thresholds (use defaults if not specified)
     */
    explicit SlowReasonAnalyzer(const Thresholds& thresholds = Thresholds());

    /**
     * @brief Analyze current sync state and detect slow reasons
     *
     * Returns the most impactful slow reason detected, or NONE if syncing normally.
     *
     * @param status Current wallet sync status
     * @param chain_db Chain database (for disk metrics)
     * @param mempool Mempool (for congestion metrics)
     * @param peer_manager Peer manager (for peer quality)
     * @param reorg_detector Reorg detector (for recent reorgs)
     * @param current_time_ms Current timestamp
     * @return Primary slow reason with details
     */
    SlowReasonInfo Analyze(
        const WalletSyncStatus& status,
        const ChainDB* chain_db,
        const Mempool* mempool,
        const PeerManager* peer_manager,
        const ReorgDetector* reorg_detector,
        uint64_t current_time_ms
    );

    /**
     * @brief Get all detected slow reasons (ranked by impact)
     *
     * Returns all applicable slow reasons, sorted by impact_factor (highest first).
     * Useful for detailed diagnostics.
     *
     * @return Vector of slow reasons, most impactful first
     */
    std::vector<SlowReasonInfo> GetAllReasons() const {
        return detected_reasons_;
    }

    /**
     * @brief Update thresholds at runtime
     */
    void SetThresholds(const Thresholds& thresholds) {
        thresholds_ = thresholds;
    }

    /**
     * @brief Get current thresholds
     */
    const Thresholds& GetThresholds() const {
        return thresholds_;
    }

private:
    /**
     * @brief Check if network is in IBD (many peers syncing)
     */
    std::optional<SlowReasonInfo> CheckNetworkIBD(
        const WalletSyncStatus& status,
        const PeerManager* peer_manager
    );

    /**
     * @brief Check for low peer quality
     */
    std::optional<SlowReasonInfo> CheckLowPeerQuality(
        const PeerManager* peer_manager
    );

    /**
     * @brief Check for disk I/O bottleneck
     */
    std::optional<SlowReasonInfo> CheckDiskBound(
        const ChainDB* chain_db
    );

    /**
     * @brief Check for high mempool pressure
     */
    std::optional<SlowReasonInfo> CheckHighMempoolPressure(
        const Mempool* mempool
    );

    /**
     * @brief Check if recovering from reorg
     */
    std::optional<SlowReasonInfo> CheckReorgRecovery(
        const ReorgDetector* reorg_detector,
        uint64_t current_time_ms
    );

    /**
     * @brief Check if wallet rescan is in progress
     */
    std::optional<SlowReasonInfo> CheckWalletRescan(
        const WalletSyncStatus& status
    );

    /**
     * @brief Calculate impact factor (0.0-1.0) for a slow reason
     *
     * Higher impact = more significant slowdown
     */
    double CalculateImpact(const SlowReasonInfo& info) const;

    Thresholds thresholds_;
    std::vector<SlowReasonInfo> detected_reasons_;  // Most recent analysis results
};

} // namespace dinero
