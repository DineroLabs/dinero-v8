/**
 * Phase N.2: Header Sync State Machine
 *
 * Manages header-only network synchronization with explicit state tracking.
 *
 * Responsibilities:
 * - Track local best header vs peer-claimed best headers
 * - Request headers from peers when behind
 * - Process incoming headers via HeaderChainSelector
 * - Persist accepted headers via HeaderStore
 * - Handle peer timeouts and invalid headers
 *
 * NOT responsible for:
 * - Block body download (Phase N.3+)
 * - UTXO validation
 * - ChainDB activation
 * - Mempool updates
 */

#pragma once

#include "primitives/uint256.h"
#include "primitives/block.h"
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <functional>

namespace dinero {
namespace consensus {

// Forward declarations
class HeaderChainSelector;
class HeaderStore;

// ============================================================================
// Header Sync State
// ============================================================================

enum class HeaderSyncState {
    IDLE,                   // Not syncing, up to date
    REQUESTING_HEADERS,     // Waiting for headers response from peer
    PROCESSING_HEADERS,     // Validating and storing received headers
    STALLED,               // Active sync peer has stalled (timeout exceeded)
    CAUGHT_UP              // Downloaded all known headers, waiting for new announcements
};

// ============================================================================
// Peer Switch Reason
// ============================================================================

enum class PeerSwitchReason {
    STALL_TIMEOUT,         // Peer exceeded header download timeout
    INVALID_HEADERS,       // Peer sent headers that failed validation
    PEER_DISCONNECT,       // Peer disconnected voluntarily
    SYNC_COMPLETE,         // Received <2000 headers, caught up with peer
    NO_PROGRESS           // Peer not making progress on sync
};

// ============================================================================
// Peer Header State
// ============================================================================

struct PeerHeaderInfo {
    uint256 best_hash;              // Hash peer claims as their best
    uint32_t best_height;           // Height peer claims as their best
    uint64_t last_request_time;     // Timestamp of last getheaders sent (ms)
    uint64_t last_response_time;    // Timestamp of last headers received (ms)
    uint64_t sync_start_time;       // Timestamp when sync started with this peer (ms)
    uint64_t timeout_deadline;      // Timestamp when peer will be considered stalled (ms)
    uint32_t expected_headers_remaining; // Estimated headers remaining to download
    bool is_stalled;                // True if peer stopped responding
    bool is_misbehaving;            // True if peer sent invalid headers
    bool is_outbound;               // True if outbound connection (prefer for sync)

    PeerHeaderInfo()
        : best_height(0)
        , last_request_time(0)
        , last_response_time(0)
        , sync_start_time(0)
        , timeout_deadline(0)
        , expected_headers_remaining(0)
        , is_stalled(false)
        , is_misbehaving(false)
        , is_outbound(false)
    {
        best_hash.SetNull();
    }
};

// ============================================================================
// Header Sync Manager
// ============================================================================

class HeaderSyncManager {
public:
    explicit HeaderSyncManager(
        HeaderChainSelector* chain_selector,
        HeaderStore* header_store = nullptr
    );
    ~HeaderSyncManager();

    // ========================================================================
    // State Machine Control
    // ========================================================================

    /**
     * Main sync tick - called periodically to drive state machine.
     * Decides when to request headers, check timeouts, etc.
     *
     * @param now_ms Optional current time in milliseconds (for testing).
     *               If 0, uses system clock.
     */
    void Tick(uint64_t now_ms = 0);

    /**
     * Get current sync state.
     */
    HeaderSyncState GetState() const { return state_; }

    /**
     * Check if we're synchronized (caught up with all known headers).
     */
    bool IsSynchronized() const { return state_ == HeaderSyncState::CAUGHT_UP; }

    // ========================================================================
    // Peer Management
    // ========================================================================

    /**
     * Register a peer and their claimed best header.
     * Called when peer sends VERSION message with start_height.
     */
    void AddPeer(uint64_t peer_id, uint32_t claimed_height, const uint256& claimed_best_hash);

    /**
     * Remove peer (disconnected).
     */
    void RemovePeer(uint64_t peer_id);

    /**
     * Update peer's claimed best header (from INV or HEADERS).
     */
    void UpdatePeerBest(uint64_t peer_id, uint32_t height, const uint256& hash);

    /**
     * Mark peer as stalled (timeout waiting for headers).
     */
    void MarkPeerStalled(uint64_t peer_id);

    /**
     * Mark peer as misbehaving (sent invalid headers).
     */
    void MarkPeerMisbehaving(uint64_t peer_id);

    /**
     * Mark peer as outbound connection (prefer for sync).
     */
    void MarkPeerOutbound(uint64_t peer_id, bool is_outbound);

    /**
     * Get best peer to request headers from (highest claimed height, not stalled).
     */
    uint64_t SelectBestPeer() const;

    // ========================================================================
    // Header Download
    // ========================================================================

    /**
     * Process received headers from a peer.
     * Returns true if headers were valid and accepted.
     * Returns false if headers were invalid (caller should punish peer).
     */
    bool ProcessHeaders(uint64_t peer_id, const std::vector<BlockHeader>& headers);

    /**
     * Generate block locator for getheaders request.
     * Returns list of hashes starting from our best header, walking back exponentially.
     */
    std::vector<uint256> GetHeaderLocator() const;

    /**
     * Check if we should request headers from a peer.
     * Returns true if peer has better headers and we're not already waiting.
     */
    bool ShouldRequestHeaders(uint64_t peer_id) const;

    /**
     * Record that we sent getheaders to a peer (for timeout tracking).
     */
    void MarkHeadersRequested(uint64_t peer_id);

    // ========================================================================
    // Statistics and Diagnostics
    // ========================================================================

    struct SyncStats {
        uint32_t local_best_height;
        uint32_t peer_best_height;
        uint32_t headers_behind;
        uint32_t active_peers;
        uint32_t stalled_peers;
        uint64_t current_sync_peer;
        HeaderSyncState state;
    };

    SyncStats GetStats() const;

    // ========================================================================
    // Peer Switch Callback
    // ========================================================================

    /**
     * Callback type for peer switch requests.
     * Called when HeaderSyncManager decides to switch peers.
     *
     * Parameters:
     *   - old_peer_id: Peer being abandoned (0 if none)
     *   - reason: Why the switch is happening
     */
    using PeerSwitchCallback = std::function<void(uint64_t old_peer_id, PeerSwitchReason reason)>;

    /**
     * Set callback for peer switch events.
     * P2P layer registers this to handle actual peer switching.
     */
    void SetPeerSwitchCallback(PeerSwitchCallback callback) {
        peer_switch_callback_ = std::move(callback);
    }

    // ========================================================================
    // Testing Support
    // ========================================================================

    /**
     * Set custom time source for testing.
     * If set, GetCurrentTimeMs() will use this instead of system clock.
     */
    void SetTimeSource(std::function<uint64_t()> time_source) {
        time_source_ = std::move(time_source);
    }

private:
    // State machine state
    HeaderSyncState state_;

    // Chain state
    HeaderChainSelector* chain_selector_;  // Not owned
    [[maybe_unused]] HeaderStore* header_store_;  // Not owned, optional

    // Peer tracking
    std::map<uint64_t, PeerHeaderInfo> peers_;

    // Active sync peer (who we're currently requesting from)
    uint64_t active_sync_peer_;

    // Peer switch callback (signals to P2P layer)
    PeerSwitchCallback peer_switch_callback_;

    // Custom time source for testing (if null, uses system clock)
    std::function<uint64_t()> time_source_;

    // Bitcoin Core timeout constants (from net_processing.cpp)
    static constexpr uint64_t HEADERS_DOWNLOAD_TIMEOUT_BASE_MS = 15 * 60 * 1000;  // 15 minutes
    static constexpr uint64_t HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER_MS = 1;        // 1ms per header

    // Bitcoin protocol constant
    static constexpr size_t MAX_HEADERS_PER_MSG = 2000;

    // ========================================================================
    // Private Helpers
    // ========================================================================

    /**
     * Transition to new state (with logging for debugging).
     */
    void TransitionTo(HeaderSyncState new_state);

    /**
     * Get current time in milliseconds since epoch.
     */
    uint64_t GetCurrentTimeMs() const;

    /**
     * Check if we're behind any peer (peer has better headers than us).
     */
    bool IsBehindPeers() const;

    /**
     * Get highest claimed height among all non-misbehaving peers.
     */
    uint32_t GetPeerBestHeight() const;

    /**
     * Calculate timeout deadline for header download.
     * Formula: 15min + (expected_headers * 1ms)
     * Bitcoin Core pattern from net_processing.cpp
     */
    uint64_t CalculateTimeout(uint32_t expected_headers) const;

    /**
     * Update timeout deadline for active sync peer.
     * Called when starting sync or receiving headers.
     */
    void UpdateSyncTimeout(uint64_t peer_id);

    /**
     * Check if active sync peer has stalled (timeout exceeded).
     * Returns true if peer should be marked as stalled.
     */
    bool CheckForStall(uint64_t now_ms);

    /**
     * Request peer switch (signals to P2P layer via callback).
     * This is intent only - actual peer switch happens in P2P code.
     */
    void RequestPeerSwitch(PeerSwitchReason reason);

    /**
     * Count available alternative peers for sync.
     * Bitcoin Core pattern: don't disconnect last peer.
     */
    size_t CountAvailablePeers() const;
};

} // namespace consensus
} // namespace dinero
