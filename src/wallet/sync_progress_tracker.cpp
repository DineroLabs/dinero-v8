#include "wallet/sync_progress_tracker.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {

// ============================================================================
// Constructor
// ============================================================================

SyncProgressTracker::SyncProgressTracker()
    : header_tracker_(std::make_unique<SyncETAEstimator>())
    , block_tracker_(std::make_unique<SyncETAEstimator>())
    , wallet_scan_tracker_(std::make_unique<SyncETAEstimator>())
{
}

// ============================================================================
// Update Tracking
// ============================================================================

void SyncProgressTracker::Update(const WalletSyncStatus& status, uint64_t timestamp_ms) {
    // Update header tracker
    header_tracker_->RecordSample(timestamp_ms, status.headers_synced);

    // Update block tracker
    block_tracker_->RecordSample(timestamp_ms, status.blocks_synced);

    // Update wallet scan tracker
    wallet_scan_tracker_->RecordSample(timestamp_ms, status.wallet_scan_height);

    dinero::g_logger.debug("SyncProgressTracker: Updated (headers=" +
                          std::to_string(status.headers_synced) + "/" +
                          std::to_string(status.headers_total) + ", blocks=" +
                          std::to_string(status.blocks_synced) + "/" +
                          std::to_string(status.blocks_total) + ", scan=" +
                          std::to_string(status.wallet_scan_height) + "/" +
                          std::to_string(status.chain_height) + ")");
}

// ============================================================================
// Phase-Aware ETA Calculation
// ============================================================================

std::optional<std::chrono::seconds> SyncProgressTracker::CalculateETA(
    const WalletSyncStatus& status,
    uint64_t current_time_ms
) const {
    // Frozen → no ETA
    if (IsFrozen()) {
        return std::nullopt;
    }

    // Already fully synced → 0 seconds
    if (status.IsFullySynced()) {
        return std::chrono::seconds(0);
    }

    // Phase-aware ETA composition
    switch (status.phase) {
        case SyncPhase::IBD: {
            // IBD: max(headers_eta, blocks_eta)
            // Wait for slower component
            auto headers_eta = CalculateHeaderETA(status, current_time_ms);
            auto blocks_eta = CalculateBlockETA(status, current_time_ms);

            // If either is unknown, can't give reliable ETA
            if (!headers_eta.has_value() || !blocks_eta.has_value()) {
                return std::nullopt;
            }

            // Return maximum (slower component determines completion)
            return std::max(headers_eta.value(), blocks_eta.value());
        }

        case SyncPhase::CATCHING_UP: {
            // CATCHING_UP: blocks_eta
            // Headers likely complete, blocks dominant
            return CalculateBlockETA(status, current_time_ms);
        }

        case SyncPhase::STEADY_STATE: {
            // STEADY_STATE: wallet_scan_eta
            // Blocks complete, scanning wallet
            return CalculateWalletScanETA(status, current_time_ms);
        }

        default:
            return std::nullopt;
    }
}

// ============================================================================
// Component ETA Calculations
// ============================================================================

std::optional<std::chrono::seconds> SyncProgressTracker::CalculateHeaderETA(
    const WalletSyncStatus& status,
    uint64_t current_time_ms
) const {
    return header_tracker_->CalculateETA(
        status.headers_synced,
        status.headers_total,
        current_time_ms
    );
}

std::optional<std::chrono::seconds> SyncProgressTracker::CalculateBlockETA(
    const WalletSyncStatus& status,
    uint64_t current_time_ms
) const {
    return block_tracker_->CalculateETA(
        status.blocks_synced,
        status.blocks_total,
        current_time_ms
    );
}

std::optional<std::chrono::seconds> SyncProgressTracker::CalculateWalletScanETA(
    const WalletSyncStatus& status,
    uint64_t current_time_ms
) const {
    return wallet_scan_tracker_->CalculateETA(
        status.wallet_scan_height,
        status.chain_height,
        current_time_ms
    );
}

// ============================================================================
// Stability Check
// ============================================================================

bool SyncProgressTracker::IsStable(SyncPhase phase, uint64_t current_time_ms) const {
    switch (phase) {
        case SyncPhase::IBD:
            // Both headers and blocks must be stable
            return header_tracker_->IsStable(current_time_ms) &&
                   block_tracker_->IsStable(current_time_ms);

        case SyncPhase::CATCHING_UP:
            // Blocks must be stable
            return block_tracker_->IsStable(current_time_ms);

        case SyncPhase::STEADY_STATE:
            // Wallet scan must be stable
            return wallet_scan_tracker_->IsStable(current_time_ms);

        default:
            return false;
    }
}

// ============================================================================
// Freeze/Unfreeze
// ============================================================================

void SyncProgressTracker::Freeze() {
    header_tracker_->Freeze();
    block_tracker_->Freeze();
    wallet_scan_tracker_->Freeze();

    dinero::g_logger.info("SyncProgressTracker: Frozen (e.g. reorg in progress)");
}

void SyncProgressTracker::Unfreeze() {
    header_tracker_->Unfreeze();
    block_tracker_->Unfreeze();
    wallet_scan_tracker_->Unfreeze();

    dinero::g_logger.info("SyncProgressTracker: Unfrozen (reorg complete)");
}

// ============================================================================
// Clear
// ============================================================================

void SyncProgressTracker::Clear() {
    header_tracker_->Clear();
    block_tracker_->Clear();
    wallet_scan_tracker_->Clear();

    dinero::g_logger.debug("SyncProgressTracker: Cleared all tracking data");
}

} // namespace dinero
