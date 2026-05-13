#pragma once

#include "daemon/iservice.h"
#include "consensus/global_utxo_set.h"
#include "daemon/p2p_message.h"
#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>

namespace dinero {
namespace daemon {

/**
 * Phase 34.5: Utreexo-Assisted Fast Sync Service
 *
 * Enables new nodes to sync in seconds by:
 * 1. Downloading only block headers (80 bytes each)
 * 2. Downloading Utreexo forest snapshot (~1KB)
 * 3. Verifying snapshot commitment against header
 * 4. Validating future blocks statelessly using proofs
 *
 * This eliminates the need to download/verify historical blocks,
 * reducing sync time from hours to seconds.
 *
 * Security Model:
 * - Headers are validated against PoW difficulty
 * - Utreexo commitment in header is PoW-protected
 * - Snapshot commitment must match header commitment
 * - Future blocks require valid Utreexo proofs
 *
 * Access Pattern:
 *   if (ctx->fast_sync->shouldFastSync()) {
 *       ctx->fast_sync->startFastSync(peer_id);
 *   }
 */
class FastSyncService : public IService {
public:
    FastSyncService();
    ~FastSyncService() override;

    // IService interface
    std::string Name() const override { return "FastSyncService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;
    bool IsHealthy() const override;
    std::string GetMetrics() const override;

    // ───────────────────────────────────────────────────────────────────────
    // Fast Sync State Machine
    // ───────────────────────────────────────────────────────────────────────

    enum class State {
        Idle,               // Not syncing
        RequestingHeaders,  // Downloading headers
        RequestingState,    // Requesting Utreexo state
        ValidatingState,    // Verifying commitment
        Completed,          // Fast sync complete
        Failed              // Fast sync failed
    };

    /**
     * @brief Check if this node should attempt fast sync
     *
     * Returns true if:
     * - --fastsync flag was passed, OR
     * - This is a fresh node with no blockchain data
     *
     * @return true if fast sync should be attempted
     */
    bool shouldFastSync() const;

    /**
     * @brief Start the fast sync process
     *
     * @param peer_id Peer to sync from
     * @return true if sync started successfully
     */
    bool startFastSync(const std::string& peer_id);

    /**
     * @brief Get current sync state
     */
    State getState() const { return state_.load(); }

    /**
     * @brief Check if fast sync is in progress
     */
    bool isSyncing() const {
        State s = state_.load();
        return s != State::Idle && s != State::Completed && s != State::Failed;
    }

    /**
     * @brief Check if fast sync completed successfully
     */
    bool isCompleted() const { return state_.load() == State::Completed; }

    // ───────────────────────────────────────────────────────────────────────
    // Message Handlers
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Handle incoming headers message
     *
     * @param peer_id Peer that sent headers
     * @param headers The block headers
     * @return true if headers were processed successfully
     */
    bool handleHeaders(const std::string& peer_id, const HeadersMessage& headers);

    /**
     * @brief Handle incoming Utreexo state message
     *
     * @param peer_id Peer that sent state
     * @param state The Utreexo state snapshot
     * @return true if state was processed successfully
     */
    bool handleUtreexoState(const std::string& peer_id, const UtreexoStateMessage& state);

    // ───────────────────────────────────────────────────────────────────────
    // Configuration
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Enable or disable fast sync
     */
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    /**
     * @brief Force fast sync (--fastsync flag)
     */
    void setForceFastSync(bool force) { force_fast_sync_ = force; }
    bool isForceFastSync() const { return force_fast_sync_; }

    /**
     * @brief Set callback for when fast sync completes
     */
    using CompletionCallback = std::function<void(bool success)>;
    void setCompletionCallback(CompletionCallback callback);

    // ───────────────────────────────────────────────────────────────────────
    // Status
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get sync progress (0.0 to 1.0)
     */
    double getProgress() const;

    /**
     * @brief Get current sync height
     */
    uint32_t getCurrentHeight() const { return current_height_.load(); }

    /**
     * @brief Get target sync height
     */
    uint32_t getTargetHeight() const { return target_height_.load(); }

    /**
     * @brief Get state as string for logging
     */
    std::string getStateString() const;

private:
    DaemonContext* ctx_{nullptr};

    // Configuration
    bool enabled_{false};
    bool force_fast_sync_{false};  // --fastsync flag

    // State machine
    std::atomic<State> state_{State::Idle};
    std::string sync_peer_id_;
    std::mutex state_mutex_;

    // Progress tracking
    std::atomic<uint32_t> current_height_{0};
    std::atomic<uint32_t> target_height_{0};
    std::atomic<uint32_t> headers_received_{0};

    // Completion callback
    CompletionCallback completion_callback_;

    // Internal methods
    void requestHeaders(const std::string& peer_id);
    void requestUtreexoState(const std::string& peer_id);
    bool verifyAndImportState(const UtreexoStateMessage& state);
    void transitionState(State new_state);
    void onSyncComplete(bool success);
};

} // namespace daemon
} // namespace dinero
