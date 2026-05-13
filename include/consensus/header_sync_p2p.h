/**
 * Phase N.2 Step 2C: Header Sync P2P Integration
 *
 * Wires HeaderSyncManager to P2P message infrastructure.
 *
 * Responsibilities:
 * - Send getheaders messages
 * - Process incoming headers messages
 * - Handle peer lifecycle (connect/disconnect)
 * - Manage peer switching when stalls detected
 *
 * Architecture:
 * - HeaderSyncManager decides policy (when to switch, timeout)
 * - HeaderSyncP2P executes actions (send messages, disconnect peers)
 * - Clean separation: policy in state machine, execution in P2P layer
 */

#pragma once

#include "consensus/header_sync.h"
#include "daemon/p2p_message.h"
#include "primitives/block.h"
#include <functional>
#include <memory>

namespace dinero {
namespace consensus {

// Forward declarations
class HeaderChainSelector;
class HeaderStore;

// ============================================================================
// Header Sync P2P Integration
// ============================================================================

class HeaderSyncP2P {
public:
    /**
     * Construct header sync P2P integration.
     *
     * @param chain_selector Header chain for validation
     * @param header_store Optional persistent storage
     */
    explicit HeaderSyncP2P(
        HeaderChainSelector* chain_selector,
        HeaderStore* header_store = nullptr
    );

    ~HeaderSyncP2P();

    // ========================================================================
    // P2P Message Handlers
    // ========================================================================

    /**
     * Handle incoming headers message from peer.
     *
     * @param peer_id Peer who sent the headers
     * @param headers_msg The headers message
     * @return true if headers were valid and accepted
     */
    bool OnHeadersMessage(uint64_t peer_id, const HeadersMessage& headers_msg);

    /**
     * Handle incoming getheaders message from peer.
     * We respond with headers we have.
     *
     * @param peer_id Peer requesting headers
     * @param getheaders_msg The getheaders request
     */
    void OnGetheadersMessage(uint64_t peer_id, const GetheadersMessage& getheaders_msg);

    // ========================================================================
    // Peer Lifecycle
    // ========================================================================

    /**
     * Handle peer connection.
     * Called when VERSION handshake completes.
     *
     * @param peer_id Peer identifier
     * @param claimed_height Height peer claims in VERSION
     * @param claimed_best_hash Best block hash peer claims
     * @param is_outbound True if we initiated the connection
     */
    void OnPeerConnected(
        uint64_t peer_id,
        uint32_t claimed_height,
        const uint256& claimed_best_hash,
        bool is_outbound
    );

    /**
     * Handle peer disconnection.
     *
     * @param peer_id Peer that disconnected
     */
    void OnPeerDisconnected(uint64_t peer_id);

    // ========================================================================
    // Sync Control
    // ========================================================================

    /**
     * Start header sync (called after IBD check, peer discovery, etc).
     */
    void StartSync();

    /**
     * Main tick - drives state machine.
     * Should be called periodically (e.g., every second).
     *
     * @param now_ms Current time in milliseconds (0 = use system time)
     */
    void Tick(uint64_t now_ms = 0);

    /**
     * Check if we're synchronized.
     */
    bool IsSynchronized() const;

    /**
     * Get sync statistics.
     */
    HeaderSyncManager::SyncStats GetStats() const;

    /**
     * Get underlying sync manager (for cases where headers are pre-parsed).
     * Phase N.3: the active P2P path may already parse headers, so bypass OnHeadersMessage.
     */
    HeaderSyncManager* GetSyncManager() { return sync_manager_.get(); }

    // ========================================================================
    // Callbacks (P2P Layer Registers These)
    // ========================================================================

    /**
     * Callback type for sending getheaders message.
     *
     * Parameters:
     *   - peer_id: Peer to send to
     *   - locator: Block locator hashes
     *   - hash_stop: Stop hash (usually null)
     */
    using SendGetheadersCallback = std::function<void(
        uint64_t peer_id,
        const std::vector<uint256>& locator,
        const uint256& hash_stop
    )>;

    /**
     * Callback type for sending headers message.
     *
     * Parameters:
     *   - peer_id: Peer to send to
     *   - headers: Headers to send
     */
    using SendHeadersCallback = std::function<void(
        uint64_t peer_id,
        const std::vector<BlockHeader>& headers
    )>;

    /**
     * Callback type for disconnecting peer.
     *
     * Parameters:
     *   - peer_id: Peer to disconnect
     *   - reason: Why we're disconnecting
     */
    using DisconnectPeerCallback = std::function<void(
        uint64_t peer_id,
        PeerSwitchReason reason
    )>;

    /**
     * Set callback for sending getheaders.
     */
    void SetSendGetheadersCallback(SendGetheadersCallback callback) {
        send_getheaders_callback_ = callback;
    }

    /**
     * Set callback for sending headers.
     */
    void SetSendHeadersCallback(SendHeadersCallback callback) {
        send_headers_callback_ = callback;
    }

    /**
     * Set callback for disconnecting peer.
     */
    void SetDisconnectPeerCallback(DisconnectPeerCallback callback) {
        disconnect_peer_callback_ = callback;
    }

    /**
     * Request headers from peer.
     * Generates locator and sends getheaders message.
     * Public so handleHeadersMessage() can send continuation after full batches.
     */
    void RequestHeadersFromPeer(uint64_t peer_id);

private:
    // Core header sync manager
    std::unique_ptr<HeaderSyncManager> sync_manager_;

    // Direct access to chain selector (for FindHeadersToSend)
    HeaderChainSelector* chain_selector_;

    // P2P callbacks
    SendGetheadersCallback send_getheaders_callback_;
    SendHeadersCallback send_headers_callback_;
    DisconnectPeerCallback disconnect_peer_callback_;

    /**
     * Handle peer switch signal from HeaderSyncManager.
     * This is where policy (state machine) meets execution (P2P).
     */
    void OnPeerSwitchRequested(uint64_t old_peer_id, PeerSwitchReason reason);

    /**
     * Convert headers message to BlockHeader vector.
     */
    std::vector<BlockHeader> ParseHeadersMessage(const HeadersMessage& msg);

    /**
     * Find headers to send in response to getheaders.
     */
    std::vector<BlockHeader> FindHeadersToSend(
        const std::vector<std::string>& locator_hashes,
        const std::string& hash_stop,
        size_t max_count = 2000
    );
};

} // namespace consensus
} // namespace dinero
