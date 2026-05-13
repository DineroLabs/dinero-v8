#pragma once

#include "wallet/reorg_event.h"
#include "wallet/wallet_sync_status.h"
#include <optional>
#include <deque>
#include <functional>

namespace dinero {

// Forward declarations
class ChainDB;

/**
 * @brief Phase W.2.4: Reorg Detection and Visibility
 *
 * Detects chain reorganizations and provides:
 * - Event tracking (depth, balance impact, affected txs)
 * - UX signals for user notifications
 * - Integration with SyncProgressTracker freeze semantics
 *
 * Design:
 * - Stateful: Tracks last known tip to detect reorgs
 * - Event history: Keeps recent reorgs for UI display
 * - Callback hooks: Notify components on reorg start/end
 */
class ReorgDetector {
public:
    /**
     * @brief Callback invoked when reorg starts
     *
     * Use to:
     * - Freeze ETA estimation
     * - Show "Reorganization in progress" UI
     * - Pause block assembly
     */
    using ReorgStartCallback = std::function<void(const ReorgEvent&)>;

    /**
     * @brief Callback invoked when reorg completes
     *
     * Use to:
     * - Unfreeze ETA estimation
     * - Update balance display
     * - Resume normal operations
     */
    using ReorgEndCallback = std::function<void(const ReorgEvent&)>;

    /**
     * @brief Constructor
     *
     * @param max_history Maximum number of recent reorg events to keep (default: 10)
     */
    explicit ReorgDetector(size_t max_history = 10);

    /**
     * @brief Check for reorganization
     *
     * Call this periodically (e.g. every block, or every sync update).
     * Compares current chain tip against last known tip to detect reorgs.
     *
     * IMPORTANT: This method only DETECTS reorgs and emits signals.
     * It does NOT calculate balance changes or modify wallet state.
     * The wallet state machine should handle balance updates in response
     * to the emitted ReorgEvent.
     *
     * @param chain_db Chain database (for tip hash and height)
     * @param current_time_ms Current timestamp
     * @return ReorgEvent if reorg detected, nullopt otherwise
     */
    std::optional<ReorgEvent> CheckForReorg(
        const ChainDB* chain_db,
        uint64_t current_time_ms
    );

    /**
     * @brief Mark current reorg as complete
     *
     * Call when reorg has finished and chain has stabilized.
     * This unfreezes ETA and clears is_in_progress flag.
     *
     * @param current_time_ms Current timestamp
     */
    void MarkReorgComplete(uint64_t current_time_ms);

    /**
     * @brief Get current reorg event (if in progress)
     *
     * @return Current reorg event, or nullopt if no reorg in progress
     */
    std::optional<ReorgEvent> GetCurrentReorg() const;

    /**
     * @brief Check if reorg is currently in progress
     */
    bool IsReorgInProgress() const {
        return current_reorg_.has_value() && current_reorg_->is_in_progress;
    }

    /**
     * @brief Get reorg history (most recent first)
     *
     * @param max_count Maximum number of events to return
     * @return Vector of recent ReorgEvent (most recent first)
     */
    std::vector<ReorgEvent> GetRecentReorgs(size_t max_count = 5) const;

    /**
     * @brief Get last reorg depth (0 if no reorgs detected)
     */
    int GetLastReorgDepth() const {
        if (reorg_history_.empty()) return 0;
        return reorg_history_.front().depth;
    }

    /**
     * @brief Register callback for reorg start
     */
    void OnReorgStart(ReorgStartCallback callback) {
        on_reorg_start_ = std::move(callback);
    }

    /**
     * @brief Register callback for reorg end
     */
    void OnReorgEnd(ReorgEndCallback callback) {
        on_reorg_end_ = std::move(callback);
    }

    /**
     * @brief Update WalletSyncStatus with reorg information
     *
     * Sets:
     * - is_reorg_in_progress
     * - last_reorg_depth
     *
     * @param status WalletSyncStatus to update
     */
    void UpdateSyncStatus(WalletSyncStatus& status) const;

    /**
     * @brief Clear all history and reset state
     */
    void Clear();

private:
    /**
     * @brief Detect reorg by comparing chain tips
     *
     * @param chain_db Chain database
     * @param current_time_ms Current timestamp
     * @return Reorg depth if detected, 0 otherwise
     */
    int DetectReorgDepth(const ChainDB* chain_db, uint64_t current_time_ms);

    // Last known chain tip hash (to detect reorgs)
    std::string last_tip_hash_;

    // Last known chain height (to detect reorgs)
    uint64_t last_tip_height_;

    // Current reorg event (if in progress)
    std::optional<ReorgEvent> current_reorg_;

    // Recent reorg history (most recent first)
    std::deque<ReorgEvent> reorg_history_;

    // Maximum history to keep
    size_t max_history_;

    // Callbacks
    ReorgStartCallback on_reorg_start_;
    ReorgEndCallback on_reorg_end_;
};

} // namespace dinero
