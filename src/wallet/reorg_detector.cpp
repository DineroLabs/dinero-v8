#include "wallet/reorg_detector.h"
#include "storage/chain_db.h"
#include "storage/tip_info.h"  // TipInfo for getTip() result
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {

// ============================================================================
// ReorgEvent Implementation
// ============================================================================

std::string ReorgEvent::GetDescription() const {
    std::ostringstream oss;

    // Primary message: reorg detected with depth
    oss << "Reorganization detected (depth: " << depth << ", " << GetSeverity() << ")";

    // Add affected tx count if > 0
    if (affected_tx_count > 0) {
        oss << " - " << affected_tx_count << " transaction";
        if (affected_tx_count > 1) oss << "s";
        oss << " affected";
    }

    // Add balance change if non-zero
    if (HasBalanceImpact()) {
        oss << "\nBalance updated: ";
        if (balance_change > 0) oss << "+";

        // Convert una to DIN (assuming 8 decimals like Bitcoin)
        double din_change = static_cast<double>(balance_change) / 100000000.0;
        oss << std::fixed << std::setprecision(8) << din_change << " DIN";
        oss << " due to reorganization";
    }

    return oss.str();
}

// ============================================================================
// ReorgDetector Constructor
// ============================================================================

ReorgDetector::ReorgDetector(size_t max_history)
    : last_tip_hash_("")
    , last_tip_height_(0)
    , max_history_(max_history)
{
}

// ============================================================================
// Reorg Detection
// ============================================================================

std::optional<ReorgEvent> ReorgDetector::CheckForReorg(
    const ChainDB* chain_db,
    uint64_t current_time_ms
) {
    if (!chain_db) {
        return std::nullopt;
    }

    // Detect reorg depth
    int depth = DetectReorgDepth(chain_db, current_time_ms);

    if (depth == 0) {
        // No reorg detected
        return std::nullopt;
    }

    // Reorg detected! Create event (signal only - no balance calculation)
    // Balance and transaction impact are calculated by the wallet state machine
    // when it processes this event via the callback
    ReorgEvent event;
    event.detected_at_height = last_tip_height_;  // Height before reorg
    event.depth = depth;
    event.timestamp_ms = current_time_ms;
    event.is_in_progress = true;
    event.balance_change = 0;        // Set by wallet state machine
    event.affected_tx_count = 0;     // Set by wallet state machine

    // Store as current reorg
    current_reorg_ = event;

    // Log event
    dinero::g_logger.warning("ReorgDetector: " + event.GetDescription());

    // Invoke callback - wallet state machine handles balance/tx updates
    if (on_reorg_start_) {
        on_reorg_start_(event);
    }

    return event;
}

int ReorgDetector::DetectReorgDepth(const ChainDB* chain_db, uint64_t current_time_ms) {
    // Get current chain tip
    uint64_t current_height = 0;
    std::string current_tip_hash = "";

    if (chain_db) {
        auto tip_opt = chain_db->getTip();
        if (tip_opt) {
            current_height = static_cast<uint64_t>(tip_opt->height);
            current_tip_hash = tip_opt->hash.ToString();
        }
    }

    // If this is first check, just record tip and return
    if (last_tip_hash_.empty()) {
        last_tip_hash_ = current_tip_hash;
        last_tip_height_ = current_height;
        return 0;
    }

    // Check if tip hash changed
    if (current_tip_hash == last_tip_hash_) {
        // Same tip, no reorg
        last_tip_height_ = current_height;
        return 0;
    }

    // Tip hash changed! This could be:
    // 1. Normal progression (new block added)
    // 2. Reorg (block(s) unwound)

    if (current_height > last_tip_height_) {
        // Normal progression - height increased
        last_tip_hash_ = current_tip_hash;
        last_tip_height_ = current_height;
        return 0;
    }

    // Height decreased or stayed same with different hash → REORG!
    int depth = static_cast<int>(last_tip_height_ - current_height);

    // Even if height stayed same, if hash changed, treat as depth 1 reorg
    if (depth == 0) {
        depth = 1;
    }

    dinero::g_logger.warning("ReorgDetector: Detected reorg (depth=" +
                            std::to_string(depth) +
                            ", old_height=" + std::to_string(last_tip_height_) +
                            ", new_height=" + std::to_string(current_height) + ")");

    // Update tracking (after logging old values)
    last_tip_hash_ = current_tip_hash;
    last_tip_height_ = current_height;

    return depth;
}

// ============================================================================
// Reorg Completion
// ============================================================================

void ReorgDetector::MarkReorgComplete(uint64_t current_time_ms) {
    if (!current_reorg_.has_value()) {
        return;  // No reorg in progress
    }

    // Mark reorg as complete
    current_reorg_->is_in_progress = false;

    // Add to history (most recent first)
    reorg_history_.push_front(*current_reorg_);

    // Trim history to max size
    while (reorg_history_.size() > max_history_) {
        reorg_history_.pop_back();
    }

    dinero::g_logger.info("ReorgDetector: Reorg complete (depth=" +
                         std::to_string(current_reorg_->depth) + ")");

    // Invoke callback
    if (on_reorg_end_) {
        on_reorg_end_(*current_reorg_);
    }

    // Clear current reorg
    current_reorg_.reset();
}

// ============================================================================
// Query Methods
// ============================================================================

std::optional<ReorgEvent> ReorgDetector::GetCurrentReorg() const {
    return current_reorg_;
}

std::vector<ReorgEvent> ReorgDetector::GetRecentReorgs(size_t max_count) const {
    std::vector<ReorgEvent> result;

    size_t count = std::min(max_count, reorg_history_.size());
    for (size_t i = 0; i < count; ++i) {
        result.push_back(reorg_history_[i]);
    }

    return result;
}

void ReorgDetector::UpdateSyncStatus(WalletSyncStatus& status) const {
    status.is_reorg_in_progress = IsReorgInProgress();
    status.last_reorg_depth = GetLastReorgDepth();
}

// ============================================================================
// Clear
// ============================================================================

void ReorgDetector::Clear() {
    last_tip_hash_.clear();
    last_tip_height_ = 0;
    current_reorg_.reset();
    reorg_history_.clear();

    dinero::g_logger.debug("ReorgDetector: Cleared all state");
}

} // namespace dinero
