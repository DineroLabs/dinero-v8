/**
 * Phase N.2: Header Sync State Machine - Implementation
 */

#include "consensus/header_sync.h"
#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include <algorithm>
#include <iostream>

namespace dinero {
namespace consensus {

// ============================================================================
// HeaderSyncManager Implementation
// ============================================================================

HeaderSyncManager::HeaderSyncManager(
    HeaderChainSelector* chain_selector,
    HeaderStore* header_store
)
    : state_(HeaderSyncState::IDLE)
    , chain_selector_(chain_selector)
    , header_store_(header_store)
    , active_sync_peer_(0)
{
    if (!chain_selector_) {
        throw std::invalid_argument("HeaderSyncManager requires non-null HeaderChainSelector");
    }
}

HeaderSyncManager::~HeaderSyncManager() {
    // chain_selector_ and header_store_ are not owned
}

HeaderSyncState HeaderSyncManager::GetState() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return state_;
}

bool HeaderSyncManager::IsSynchronized() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return state_ == HeaderSyncState::CAUGHT_UP;
}

// ============================================================================
// State Machine Control
// ============================================================================

void HeaderSyncManager::Tick(uint64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Use provided time for testing, otherwise get system time
    uint64_t now = (now_ms > 0) ? now_ms : GetCurrentTimeMs();

    // Check for stalls in all states except IDLE
    if (state_ != HeaderSyncState::IDLE && state_ != HeaderSyncState::CAUGHT_UP) {
        if (CheckForStall(now)) {
            // Peer has stalled - transition to STALLED state
            TransitionTo(HeaderSyncState::STALLED);

            // Always signal stall to P2P layer (it decides what to do)
            RequestPeerSwitch(PeerSwitchReason::STALL_TIMEOUT);

            // If we have alternatives, go back to IDLE to try another peer
            if (CountAvailablePeers() > 0) {
                TransitionTo(HeaderSyncState::IDLE);
            }
            // else: stay in STALLED state, P2P layer handles recovery
        }
    }

    switch (state_) {
        case HeaderSyncState::IDLE:
            // Check if any peer has better headers
            // Note: Tick() identifies sync candidates but doesn't transition state.
            // The P2P layer calls BeginHeadersRequest() immediately before it
            // sends getheaders, reserving ownership and transitioning state.
            // This separation ensures ShouldRequestHeaders() returns true until
            // the actual request is sent.
            break;

        case HeaderSyncState::REQUESTING_HEADERS:
            // Stall detection handled above
            // This state waits for headers to arrive
            break;

        case HeaderSyncState::PROCESSING_HEADERS:
            // Processing should be fast, automatically transitions back to IDLE/REQUESTING
            // This state exists for observability during batch processing
            break;

        case HeaderSyncState::STALLED:
            // Waiting for peer switch
            // Will transition back to IDLE once new peer selected
            break;

        case HeaderSyncState::CAUGHT_UP:
            // Check if peers announce new headers
            if (IsBehindPeers()) {
                TransitionTo(HeaderSyncState::IDLE);
            }
            break;
    }
}

// ============================================================================
// Peer Management
// ============================================================================

void HeaderSyncManager::AddPeer(uint64_t peer_id, uint32_t claimed_height, const uint256& claimed_best_hash) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PeerHeaderInfo info;
    info.best_height = claimed_height;
    info.best_hash = claimed_best_hash;
    info.last_request_time = 0;
    info.last_response_time = GetCurrentTimeMs();
    info.is_stalled = false;
    info.is_misbehaving = false;

    peers_[peer_id] = info;

    // If we're idle and this peer is ahead, we might want to sync
    if (state_ == HeaderSyncState::IDLE || state_ == HeaderSyncState::CAUGHT_UP) {
        // #441: copy under the selector's lock — GetBestHeader() returns a raw
        // pointer after releasing it, and a reorg can demote the former best
        // header to an evictable side-branch tip.
        HeaderIndexEntry best_copy{};
        uint32_t our_height =
            chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;
        if (claimed_height > our_height) {
            // New peer is ahead, trigger sync
            TransitionTo(HeaderSyncState::IDLE);
        }
    }
}

void HeaderSyncManager::RemovePeer(uint64_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    peers_.erase(peer_id);

    // If this was our active sync peer, go back to IDLE
    if (active_sync_peer_ == peer_id) {
        active_sync_peer_ = 0;
        TransitionTo(HeaderSyncState::IDLE);
    }
}

void HeaderSyncManager::UpdatePeerBest(uint64_t peer_id, uint32_t height, const uint256& hash) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return;  // Unknown peer
    }

    it->second.best_height = height;
    it->second.best_hash = hash;

    // If peer announced better headers, might need to sync
    if (state_ == HeaderSyncState::CAUGHT_UP) {
        // #441: copy under the selector's lock — GetBestHeader() returns a raw
        // pointer after releasing it, and a reorg can demote the former best
        // header to an evictable side-branch tip.
        HeaderIndexEntry best_copy{};
        uint32_t our_height =
            chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;
        if (height > our_height) {
            TransitionTo(HeaderSyncState::IDLE);
        }
    }
}

void HeaderSyncManager::MarkPeerStalled(uint64_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.is_stalled = true;
    }
}

void HeaderSyncManager::MarkPeerMisbehaving(uint64_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.is_misbehaving = true;
    }

    // If this was our active sync peer, request switch
    if (active_sync_peer_ == peer_id) {
        const_cast<HeaderSyncManager*>(this)->RequestPeerSwitch(PeerSwitchReason::INVALID_HEADERS);
    }
}

void HeaderSyncManager::MarkPeerOutbound(uint64_t peer_id, bool is_outbound) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.is_outbound = is_outbound;
    }
}

uint64_t HeaderSyncManager::SelectBestPeer() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    uint64_t best_peer = 0;
    uint32_t best_height = 0;
    bool best_is_outbound = false;

    for (const auto& pair : peers_) {
        uint64_t peer_id = pair.first;
        const PeerHeaderInfo& info = pair.second;

        // Skip stalled or misbehaving peers
        if (info.is_stalled || info.is_misbehaving) {
            continue;
        }

        // Bitcoin Core pattern: prefer outbound peers for eclipse resistance
        bool should_replace = false;

        if (info.best_height > best_height) {
            // Higher height always wins
            should_replace = true;
        } else if (info.best_height == best_height) {
            // Same height: prefer outbound over inbound
            if (info.is_outbound && !best_is_outbound) {
                should_replace = true;
            }
        }

        if (should_replace) {
            best_height = info.best_height;
            best_peer = peer_id;
            best_is_outbound = info.is_outbound;
        }
    }

    return best_peer;
}

// ============================================================================
// Header Download
// ============================================================================

bool HeaderSyncManager::ProcessHeaders(uint64_t peer_id, const std::vector<BlockHeader>& headers) {
    return ProcessHeadersWithResult(peer_id, headers).accepted;
}

HeaderSyncManager::ProcessResult HeaderSyncManager::ProcessHeadersWithResult(
    uint64_t peer_id, const std::vector<BlockHeader>& headers) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ProcessResult result;
    const bool owns_request = active_sync_peer_ == peer_id;
    const bool may_drive_state = owns_request || active_sync_peer_ == 0;
    // Consuming this peer's response supersedes its previous continuation.
    // A late response must not redirect another peer's outstanding request.
    if (may_drive_state) {
        auto peer = peers_.find(peer_id);
        if (peer != peers_.end()) peer->second.continuation_hash.reset();
    }

    if (headers.empty()) {
        // Empty headers message means peer has no more headers to send
        // for the supplied locator. Do not collapse the peer's advertised best
        // height to our local selector height; that turns a local header-state
        // bug into misleading peer telemetry.
        auto peer_it = peers_.find(peer_id);
        if (peer_it != peers_.end()) {
            peer_it->second.last_response_time = GetCurrentTimeMs();
        }

        // Check if we're caught up with all peers
        if (may_drive_state) {
            if (owns_request) {
                active_sync_peer_ = 0;
            }
            if (!IsBehindPeers()) {
                TransitionTo(HeaderSyncState::CAUGHT_UP);
            } else {
                TransitionTo(HeaderSyncState::IDLE);
            }
        }

        result.accepted = true;
        return result;
    }

    // Transition to processing state
    if (may_drive_state) {
        TransitionTo(HeaderSyncState::PROCESSING_HEADERS);
    }

    // Update peer's last response time and timeout deadline
    auto peer_it = peers_.find(peer_id);
    if (peer_it != peers_.end()) {
        peer_it->second.last_response_time = GetCurrentTimeMs();
    }

    // Validate and add headers one by one
    size_t accepted = 0;
    for (const BlockHeader& header : headers) {
        // Validate via HeaderChainSelector
        const auto add_result = chain_selector_->AddHeaderWithResult(header);
        if (add_result == HeaderChainSelector::AddResult::REJECTED) {
            const bool missing_parent_locally =
                (accepted == 0 && !chain_selector_->ContainsHeader(header.prev_block_hash));

            if (missing_parent_locally) {
                std::cerr << "[HeaderSyncManager] Local header gap: missing parent "
                          << header.prev_block_hash.GetHex().substr(0, 16)
                          << "... for first incoming header "
                          << header.GetHash().GetHex().substr(0, 16)
                          << "... from peer " << peer_id << std::endl;
                if (owns_request) {
                    active_sync_peer_ = 0;
                }
                if (may_drive_state) {
                    TransitionTo(HeaderSyncState::IDLE);
                }
            } else {
                // Invalid header payload/chain from peer - mark peer as misbehaving
                MarkPeerMisbehaving(peer_id);
                // A synchronous peer-switch callback may already have reserved
                // another request. Do not overwrite that newer state.
                if (may_drive_state && active_sync_peer_ == 0) {
                    TransitionTo(HeaderSyncState::IDLE);
                }
            }
            return result;
        }

        accepted++;
        if (add_result == HeaderChainSelector::AddResult::INSERTED) {
            result.inserted++;
        } else {
            result.duplicates++;
        }
    }

    // All headers accepted
    // Note: HeaderChainSelector auto-persists via HeaderStore if configured

    // Validation is complete. Release only the request this response owns.
    if (owns_request) {
        active_sync_peer_ = 0;
    }

    // Check if we got a full batch (2000 headers)
    // If so, there might be more headers available
    if (headers.size() >= MAX_HEADERS_PER_MSG) {
        result.request_more = may_drive_state;
        if (may_drive_state) {
            TransitionTo(HeaderSyncState::IDLE);
        }
    } else {
        // Partial batch (< 2000 headers) - check if we're truly caught up with this peer
        // #441: copy under the selector's lock — GetBestHeader() returns a raw
        // pointer after releasing it, and a reorg can demote the former best
        // header to an evictable side-branch tip.
        HeaderIndexEntry best_copy{};
        uint32_t our_height =
            chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;

        auto peer_it = peers_.find(peer_id);
        uint32_t peer_claimed_height = (peer_it != peers_.end()) ? peer_it->second.best_height : 0;

        if (!may_drive_state) {
            // Keep the newer request's state and ownership intact.
        } else if (our_height >= peer_claimed_height) {
            // Truly caught up with this peer. The next periodic Tick selects a
            // different peer if another one advertises a higher frontier.
            if (IsBehindPeers()) {
                TransitionTo(HeaderSyncState::IDLE);
            } else {
                TransitionTo(HeaderSyncState::CAUGHT_UP);
            }
        } else {
            // The protocol does not send another batch unsolicited. Ask again
            // from the new best-header frontier through the serialized request
            // path instead of claiming a request is active before one is sent.
            result.request_more = true;
            TransitionTo(HeaderSyncState::IDLE);
        }
    }

    if (result.request_more && peer_it != peers_.end()) {
        // Strict IBD exposed repeated genesis..2000 batches when this valid
        // fork had not yet overtaken the best-work tip. Continue from the
        // received frontier; best-work selection remains the selector's job.
        peer_it->second.continuation_hash = headers.back().GetHash();
    }
    result.accepted = true;
    return result;
}

std::vector<uint256> HeaderSyncManager::GetHeaderLocator() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // #441: built entirely under the selector's lock.
    //
    // The previous implementation took the tip via GetBestHeader() and then
    // walked back with repeated GetHeaderAtHeight() calls. That was unsafe
    // twice: both accessors return raw pointers after releasing the lock (and a
    // reorg can demote the former best header to an evictable side-branch tip),
    // and taking the lock once per step meant a concurrent reorg could produce a
    // locator mixing hashes from different chain states.
    return chain_selector_->BuildLocatorCopy(10);
}

bool HeaderSyncManager::ShouldRequestHeaders(uint64_t peer_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Don't request if we're already waiting for headers
    if (state_ == HeaderSyncState::REQUESTING_HEADERS && active_sync_peer_ != 0) {
        return false;
    }

    // Don't request if we're processing headers
    if (state_ == HeaderSyncState::PROCESSING_HEADERS) {
        return false;
    }

    // Check if peer has better headers than us
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return false;  // Unknown peer
    }

    const PeerHeaderInfo& info = it->second;
    if (info.is_stalled || info.is_misbehaving) {
        return false;  // Don't request from bad peers
    }

    // #441: copy under the selector's lock (see note above).
    HeaderIndexEntry best_copy{};
    uint32_t our_height =
        chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;

    return info.best_height > our_height;
}

std::optional<std::vector<uint256>> HeaderSyncManager::BeginHeadersRequest(
    uint64_t peer_id, bool probe) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const auto peer_it = peers_.find(peer_id);
    if (peer_it == peers_.end() || peer_it->second.is_stalled ||
        peer_it->second.is_misbehaving) {
        return std::nullopt;
    }

    // Exactly one network request may own the continuation at a time. This is
    // global, not merely per peer: a response from a second peer can otherwise
    // reset a valid continuation from the first one.
    if (active_sync_peer_ != 0) {
        return std::nullopt;
    }

    if (!probe && !ShouldRequestHeaders(peer_id)) {
        return std::nullopt;
    }

    auto locator = GetHeaderLocator();
    if (locator.empty()) {
        return std::nullopt;
    }

    if (peer_it->second.continuation_hash &&
        chain_selector_->ContainsHeader(*peer_it->second.continuation_hash) &&
        locator.front() != *peer_it->second.continuation_hash) {
        locator.insert(locator.begin(), *peer_it->second.continuation_hash);
    }

    peer_it->second.last_request_time = GetCurrentTimeMs();
    active_sync_peer_ = peer_id;
    UpdateSyncTimeout(peer_id);
    TransitionTo(HeaderSyncState::REQUESTING_HEADERS);
    return locator;
}

void HeaderSyncManager::MarkHeadersRequestFailed(uint64_t peer_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (active_sync_peer_ == peer_id) {
        active_sync_peer_ = 0;
        TransitionTo(HeaderSyncState::IDLE);
    }
}

// ============================================================================
// Statistics and Diagnostics
// ============================================================================

HeaderSyncManager::SyncStats HeaderSyncManager::GetStats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    SyncStats stats;

    // #441: copy under the selector's lock (see note above).
    HeaderIndexEntry best_copy{};
    stats.local_best_height =
        chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;
    stats.peer_best_height = GetPeerBestHeight();
    stats.headers_behind = (stats.peer_best_height > stats.local_best_height)
                          ? (stats.peer_best_height - stats.local_best_height)
                          : 0;

    stats.active_peers = 0;
    stats.stalled_peers = 0;
    for (const auto& pair : peers_) {
        if (!pair.second.is_misbehaving) {
            if (pair.second.is_stalled) {
                stats.stalled_peers++;
            } else {
                stats.active_peers++;
            }
        }
    }

    stats.current_sync_peer = active_sync_peer_;
    stats.state = state_;

    return stats;
}

// ============================================================================
// Private Helpers
// ============================================================================

void HeaderSyncManager::TransitionTo(HeaderSyncState new_state) {
    if (state_ != new_state) {
        state_ = new_state;
    }
}

uint64_t HeaderSyncManager::GetCurrentTimeMs() const {
    // Use custom time source if set (for testing)
    if (time_source_) {
        return time_source_();
    }
    // Default to system clock
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

bool HeaderSyncManager::IsBehindPeers() const {
    // #441: copy under the selector's lock (see note above).
    HeaderIndexEntry best_copy{};
    uint32_t our_height =
        chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;

    for (const auto& pair : peers_) {
        const PeerHeaderInfo& info = pair.second;

        // Skip misbehaving peers
        if (info.is_misbehaving) {
            continue;
        }

        // If any non-misbehaving peer claims higher height, we're behind
        if (info.best_height > our_height) {
            return true;
        }
    }

    return false;
}

uint32_t HeaderSyncManager::GetPeerBestHeight() const {
    uint32_t max_height = 0;

    for (const auto& pair : peers_) {
        const PeerHeaderInfo& info = pair.second;

        // Skip misbehaving peers
        if (info.is_misbehaving) {
            continue;
        }

        if (info.best_height > max_height) {
            max_height = info.best_height;
        }
    }

    return max_height;
}

uint64_t HeaderSyncManager::CalculateTimeout(uint32_t expected_headers) const {
    // Bitcoin Core formula: 15min + (expected_headers * 1ms)
    return HEADERS_DOWNLOAD_TIMEOUT_BASE_MS +
           (expected_headers * HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER_MS);
}

void HeaderSyncManager::UpdateSyncTimeout(uint64_t peer_id) {
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return;
    }

    PeerHeaderInfo& info = it->second;
    uint64_t now = GetCurrentTimeMs();

    // Calculate expected headers remaining
    // #441: copy under the selector's lock (see note above).
    HeaderIndexEntry best_copy{};
    uint32_t our_height =
        chain_selector_->GetBestHeaderCopy(best_copy) ? best_copy.height : 0;
    uint32_t expected_headers = (info.best_height > our_height)
                              ? (info.best_height - our_height)
                              : 0;

    // Set sync start time if not already set
    if (info.sync_start_time == 0) {
        info.sync_start_time = now;
    }

    // Update expected headers and timeout deadline
    info.expected_headers_remaining = expected_headers;
    uint64_t timeout_duration = CalculateTimeout(expected_headers);
    info.timeout_deadline = now + timeout_duration;
}

bool HeaderSyncManager::CheckForStall(uint64_t now_ms) {
    if (active_sync_peer_ == 0) {
        return false;  // No active sync peer
    }

    auto it = peers_.find(active_sync_peer_);
    if (it == peers_.end()) {
        // Peer disconnected
        return true;
    }

    const PeerHeaderInfo& info = it->second;

    // Check if timeout deadline exceeded
    if (info.timeout_deadline > 0 && now_ms > info.timeout_deadline) {
        // Peer has stalled
        return true;
    }

    return false;
}

void HeaderSyncManager::RequestPeerSwitch(PeerSwitchReason reason) {
    uint64_t old_peer = active_sync_peer_;

    // Mark old peer as stalled if reason is timeout
    if (reason == PeerSwitchReason::STALL_TIMEOUT && old_peer != 0) {
        MarkPeerStalled(old_peer);
    }

    // Clear active sync peer
    active_sync_peer_ = 0;

    // Invoke callback if registered (signals to P2P layer)
    if (peer_switch_callback_) {
        peer_switch_callback_(old_peer, reason);
    }
}

size_t HeaderSyncManager::CountAvailablePeers() const {
    size_t count = 0;

    for (const auto& pair : peers_) {
        const PeerHeaderInfo& info = pair.second;

        // Skip stalled or misbehaving peers
        if (info.is_stalled || info.is_misbehaving) {
            continue;
        }

        // Skip current sync peer
        if (pair.first == active_sync_peer_) {
            continue;
        }

        count++;
    }

    return count;
}

} // namespace consensus
} // namespace dinero
