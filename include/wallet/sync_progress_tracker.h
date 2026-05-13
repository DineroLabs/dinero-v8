#pragma once

#include "wallet/sync_eta_estimator.h"
#include "wallet/wallet_sync_status.h"
#include <memory>
#include <chrono>

namespace dinero {

/**
 * @brief Phase W.2.2: Sync Progress Tracker (ETA + Rate Tracking)
 *
 * Manages three independent rate trackers:
 * - Headers sync rate (headers/sec)
 * - Blocks sync rate (blocks/sec)
 * - Wallet scan rate (blocks/sec)
 *
 * Provides phase-aware ETA composition:
 * - IBD: max(headers_eta, blocks_eta)
 * - CATCHING_UP: blocks_eta
 * - STEADY_STATE: wallet_scan_eta
 *
 * Design Principles:
 * - Each component tracks independently
 * - Never average incompatible domains
 * - Phase determines which ETA to surface
 * - Freeze all during reorg
 */
class SyncProgressTracker {
public:
    SyncProgressTracker();
    ~SyncProgressTracker() = default;

    /**
     * @brief Update progress tracking with current sync status
     *
     * Call this periodically (e.g. every 1s) to track progress and rates.
     *
     * @param status Current wallet sync status
     * @param timestamp_ms Current timestamp (milliseconds)
     */
    void Update(const WalletSyncStatus& status, uint64_t timestamp_ms);

    /**
     * @brief Calculate phase-aware ETA
     *
     * Returns ETA based on current sync phase:
     * - IBD: max(headers_eta, blocks_eta)  // Wait for slower component
     * - CATCHING_UP: blocks_eta            // Headers likely complete
     * - STEADY_STATE: wallet_scan_eta      // Blocks complete, scanning wallet
     *
     * @param status Current wallet sync status
     * @param current_time_ms Current timestamp
     * @return ETA in seconds, or nullopt if unstable/unknown
     */
    std::optional<std::chrono::seconds> CalculateETA(
        const WalletSyncStatus& status,
        uint64_t current_time_ms
    ) const;

    /**
     * @brief Get header sync rate (headers/second)
     */
    double GetHeaderRate() const { return header_tracker_->GetCurrentRate(); }

    /**
     * @brief Get block sync rate (blocks/second)
     */
    double GetBlockRate() const { return block_tracker_->GetCurrentRate(); }

    /**
     * @brief Get wallet scan rate (blocks/second)
     */
    double GetWalletScanRate() const { return wallet_scan_tracker_->GetCurrentRate(); }

    /**
     * @brief Check if trackers have enough data for stable ETA
     *
     * @param phase Current sync phase
     * @param current_time_ms Current timestamp
     * @return True if relevant tracker(s) are stable
     */
    bool IsStable(SyncPhase phase, uint64_t current_time_ms) const;

    /**
     * @brief Freeze all ETA calculation (e.g. during reorg)
     *
     * While frozen, CalculateETA() will return nullopt.
     */
    void Freeze();

    /**
     * @brief Unfreeze all ETA calculation
     */
    void Unfreeze();

    /**
     * @brief Check if frozen
     */
    bool IsFrozen() const { return header_tracker_->IsFrozen(); }

    /**
     * @brief Clear all tracking data and reset state
     */
    void Clear();

private:
    /**
     * @brief Calculate ETA for headers sync
     *
     * @param status Current sync status
     * @param current_time_ms Current timestamp
     * @return ETA or nullopt
     */
    std::optional<std::chrono::seconds> CalculateHeaderETA(
        const WalletSyncStatus& status,
        uint64_t current_time_ms
    ) const;

    /**
     * @brief Calculate ETA for blocks sync
     *
     * @param status Current sync status
     * @param current_time_ms Current timestamp
     * @return ETA or nullopt
     */
    std::optional<std::chrono::seconds> CalculateBlockETA(
        const WalletSyncStatus& status,
        uint64_t current_time_ms
    ) const;

    /**
     * @brief Calculate ETA for wallet scan
     *
     * @param status Current sync status
     * @param current_time_ms Current timestamp
     * @return ETA or nullopt
     */
    std::optional<std::chrono::seconds> CalculateWalletScanETA(
        const WalletSyncStatus& status,
        uint64_t current_time_ms
    ) const;

    std::unique_ptr<SyncETAEstimator> header_tracker_;       // Headers sync tracker
    std::unique_ptr<SyncETAEstimator> block_tracker_;        // Blocks sync tracker
    std::unique_ptr<SyncETAEstimator> wallet_scan_tracker_;  // Wallet scan tracker
};

} // namespace dinero
