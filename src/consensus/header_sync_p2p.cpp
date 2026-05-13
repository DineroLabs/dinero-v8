/**
 * Phase N.2 Step 2C: Header Sync P2P Integration - Implementation
 */

#include "consensus/header_sync_p2p.h"
#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include <iostream>

namespace dinero {
namespace consensus {

// ============================================================================
// HeaderSyncP2P Implementation
// ============================================================================

HeaderSyncP2P::HeaderSyncP2P(
    HeaderChainSelector* chain_selector,
    HeaderStore* header_store
)
    : sync_manager_(std::make_unique<HeaderSyncManager>(chain_selector, header_store))
    , chain_selector_(chain_selector)
{
    // Register peer switch callback
    sync_manager_->SetPeerSwitchCallback(
        [this](uint64_t old_peer_id, PeerSwitchReason reason) {
            OnPeerSwitchRequested(old_peer_id, reason);
        }
    );
}

HeaderSyncP2P::~HeaderSyncP2P() {
    // unique_ptr handles cleanup
}

// ============================================================================
// P2P Message Handlers
// ============================================================================

bool HeaderSyncP2P::OnHeadersMessage(uint64_t peer_id, const HeadersMessage& headers_msg) {
    // Parse headers from message
    std::vector<BlockHeader> headers = ParseHeadersMessage(headers_msg);

    // Process through sync manager
    bool accepted = sync_manager_->ProcessHeaders(peer_id, headers);

    if (!accepted) {
        // Headers were invalid - peer will be marked as misbehaving
        // P2P layer should handle punishment (disconnect/ban)
        std::cerr << "[HeaderSyncP2P] Invalid headers from peer " << peer_id << std::endl;
        return false;
    }

    // Check if we need to request more headers
    if (headers.size() >= 2000) {
        // Full batch received - request more from same peer
        RequestHeadersFromPeer(peer_id);
    }

    return true;
}

void HeaderSyncP2P::OnGetheadersMessage(uint64_t peer_id, const GetheadersMessage& getheaders_msg) {
    // Find headers to send based on locator
    std::vector<BlockHeader> headers_to_send = FindHeadersToSend(
        getheaders_msg.block_locator_hashes,
        getheaders_msg.hash_stop,
        2000  // Max headers per message
    );

    // Send headers response
    if (send_headers_callback_) {
        send_headers_callback_(peer_id, headers_to_send);
    }
}

// ============================================================================
// Peer Lifecycle
// ============================================================================

void HeaderSyncP2P::OnPeerConnected(
    uint64_t peer_id,
    uint32_t claimed_height,
    const uint256& claimed_best_hash,
    bool is_outbound
) {
    // Register peer with sync manager
    sync_manager_->AddPeer(peer_id, claimed_height, claimed_best_hash);
    sync_manager_->MarkPeerOutbound(peer_id, is_outbound);

    std::cout << "[HeaderSyncP2P] Peer " << peer_id
              << " connected (height=" << claimed_height
              << ", outbound=" << is_outbound << ")" << std::endl;
}

void HeaderSyncP2P::OnPeerDisconnected(uint64_t peer_id) {
    // Remove peer from sync manager
    sync_manager_->RemovePeer(peer_id);

    std::cout << "[HeaderSyncP2P] Peer " << peer_id << " disconnected" << std::endl;
}

// ============================================================================
// Sync Control
// ============================================================================

void HeaderSyncP2P::StartSync() {
    std::cout << "[HeaderSyncP2P] Starting header sync..." << std::endl;

    // State machine will automatically request headers if behind
    // Just trigger a tick to start the process
    Tick();
}

void HeaderSyncP2P::Tick(uint64_t now_ms) {
    // Drive state machine (handles stall detection, peer tracking)
    sync_manager_->Tick(now_ms);

    // Check if we should request headers
    auto stats = sync_manager_->GetStats();

    // Request headers if we're IDLE and behind peers
    // MarkHeadersRequested() will transition to REQUESTING_HEADERS
    if (stats.state == HeaderSyncState::IDLE && stats.headers_behind > 0) {
        uint64_t peer_id = sync_manager_->SelectBestPeer();
        if (peer_id != 0 && sync_manager_->ShouldRequestHeaders(peer_id)) {
            RequestHeadersFromPeer(peer_id);
        }
    }
}

bool HeaderSyncP2P::IsSynchronized() const {
    return sync_manager_->IsSynchronized();
}

HeaderSyncManager::SyncStats HeaderSyncP2P::GetStats() const {
    return sync_manager_->GetStats();
}

// ============================================================================
// Private Helpers
// ============================================================================

void HeaderSyncP2P::RequestHeadersFromPeer(uint64_t peer_id) {
    // Generate block locator
    std::vector<uint256> locator = sync_manager_->GetHeaderLocator();

    // Mark that we've requested headers (for timeout tracking)
    sync_manager_->MarkHeadersRequested(peer_id);

    // Send getheaders message
    if (send_getheaders_callback_) {
        uint256 hash_stop;  // Null hash = get as many as possible
        hash_stop.SetNull();
        send_getheaders_callback_(peer_id, locator, hash_stop);

        std::cout << "[HeaderSyncP2P] Requested headers from peer " << peer_id
                  << " (locator size=" << locator.size() << ")" << std::endl;
    }
}

void HeaderSyncP2P::OnPeerSwitchRequested(uint64_t old_peer_id, PeerSwitchReason reason) {
    std::cout << "[HeaderSyncP2P] Peer switch requested: old_peer=" << old_peer_id
              << " reason=";

    switch (reason) {
        case PeerSwitchReason::STALL_TIMEOUT:
            std::cout << "STALL_TIMEOUT";
            break;
        case PeerSwitchReason::INVALID_HEADERS:
            std::cout << "INVALID_HEADERS";
            break;
        case PeerSwitchReason::PEER_DISCONNECT:
            std::cout << "PEER_DISCONNECT";
            break;
        case PeerSwitchReason::SYNC_COMPLETE:
            std::cout << "SYNC_COMPLETE";
            break;
        case PeerSwitchReason::NO_PROGRESS:
            std::cout << "NO_PROGRESS";
            break;
    }
    std::cout << std::endl;

    // Handle disconnect if needed (not for SYNC_COMPLETE)
    if (reason != PeerSwitchReason::SYNC_COMPLETE && old_peer_id != 0) {
        if (disconnect_peer_callback_) {
            disconnect_peer_callback_(old_peer_id, reason);
        }
    }

    // If SYNC_COMPLETE, verify with other peers (Bitcoin Core pattern)
    if (reason == PeerSwitchReason::SYNC_COMPLETE) {
        std::cout << "[HeaderSyncP2P] Sync complete - TODO: verify with all outbound peers" << std::endl;
        // TODO Phase N.3: Query all outbound peers to verify best chain
    }

    // Select new peer and request headers
    uint64_t new_peer = sync_manager_->SelectBestPeer();
    if (new_peer != 0 && sync_manager_->ShouldRequestHeaders(new_peer)) {
        RequestHeadersFromPeer(new_peer);
    }
}

std::vector<BlockHeader> HeaderSyncP2P::ParseHeadersMessage(const HeadersMessage& msg) {
    std::vector<BlockHeader> headers;
    headers.reserve(msg.headers.size());

    for (const auto& header_data : msg.headers) {
        // Deserialize 128-byte BlockHeader from raw P2P message data
        auto header_opt = BlockHeader::Deserialize(header_data);
        if (!header_opt) {
            // Invalid header - skip this one but continue processing others
            // Note: Caller should validate the returned vector size matches expected
            continue;
        }

        // Validate reserved field (BlockHeader v1 consensus rule)
        if (!header_opt->IsReservedValid()) {
            // Reserved bytes must be zero - reject malformed header
            continue;
        }

        headers.push_back(*header_opt);
    }

    return headers;
}

std::vector<BlockHeader> HeaderSyncP2P::FindHeadersToSend(
    const std::vector<std::string>& locator_hashes,
    const std::string& hash_stop,
    size_t max_count
) {
    std::vector<BlockHeader> headers;

    if (!chain_selector_) {
        return headers;  // No chain selector available
    }

    // ========================================================================
    // STEP 1: Find the first locator hash that we have in our chain
    // ========================================================================
    // Block locator is ordered from tip to genesis, so we find the first
    // hash we know about - that's our starting point.
    const HeaderIndexEntry* start_entry = nullptr;

    for (const auto& hash_hex : locator_hashes) {
        uint256 hash = uint256::FromHexUnsafe(hash_hex);
        const HeaderIndexEntry* entry = chain_selector_->GetHeader(hash);
        if (entry) {
            start_entry = entry;
            break;
        }
    }

    // If no locator hash found, start from genesis (height 0)
    if (!start_entry) {
        start_entry = chain_selector_->GetHeaderAtHeight(0);
        if (!start_entry) {
            return headers;  // No genesis - empty chain
        }
    }

    // ========================================================================
    // STEP 2: Walk forward from start_entry, collecting headers
    // ========================================================================
    // We need headers AFTER start_entry (peer already has start_entry)
    // Walk from start_entry.height + 1 up to best header or hash_stop

    const HeaderIndexEntry* best = chain_selector_->GetBestHeader();
    if (!best) {
        return headers;
    }

    // Parse hash_stop (null or specific hash)
    uint256 stop_hash;
    if (!hash_stop.empty()) {
        stop_hash = uint256::FromHexUnsafe(hash_stop);
    }

    // Collect headers from start_height + 1 to best
    uint32_t start_height = start_entry->height + 1;
    uint32_t end_height = best->height;

    headers.reserve(std::min(static_cast<size_t>(end_height - start_height + 1), max_count));

    for (uint32_t h = start_height; h <= end_height && headers.size() < max_count; ++h) {
        const HeaderIndexEntry* entry = chain_selector_->GetHeaderAtHeight(h);
        if (!entry) {
            break;  // Gap in chain - shouldn't happen
        }

        headers.push_back(entry->header);

        // Check if we hit hash_stop
        if (!stop_hash.IsNull() && entry->hash == stop_hash) {
            break;
        }
    }

    std::cerr << "[HeaderSyncP2P] FindHeadersToSend: returning " << headers.size()
              << " headers (start_height=" << start_height << ", end_height=" << end_height << ")"
              << std::endl;

    return headers;
}

} // namespace consensus
} // namespace dinero
