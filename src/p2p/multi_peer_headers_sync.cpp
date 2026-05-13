#include "p2p/multi_peer_headers_sync.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include "consensus/pow.h"  // Phase D.4: Use consensus PoW verification
#include <algorithm>
#include <sstream>
#include <cmath>

namespace dinero {
namespace p2p {

using namespace multi_peer_sync;

// ═══════════════════════════════════════════════════════════════════════════
// PeerHeaderState Implementation
// ═══════════════════════════════════════════════════════════════════════════

void PeerHeaderState::recordValidHeaders(uint32_t count) {
    valid_headers_received += count;
    header_score = std::min(100.0, header_score + (count * VALID_HEADER_REWARD));
    last_activity = std::chrono::steady_clock::now();

    dinero::g_logger.debug("Peer " + peer_id + " +valid headers: " + std::to_string(count) +
                          " (score: " + std::to_string(header_score) + ")");
}

void PeerHeaderState::recordInvalidHeaders(uint32_t count) {
    invalid_headers_received += count;
    header_score = std::max(0.0, header_score + (count * INVALID_HEADER_PENALTY));

    dinero::g_logger.warning("Peer " + peer_id + " sent " + std::to_string(count) +
                            " invalid headers (score: " + std::to_string(header_score) + ")");
}

void PeerHeaderState::recordTimeout() {
    timeouts++;
    header_score = std::max(0.0, header_score + TIMEOUT_PENALTY);

    dinero::g_logger.warning("Peer " + peer_id + " timed out (score: " +
                            std::to_string(header_score) + ")");
}

bool PeerHeaderState::hasTimedOut() const {
    if (!has_inflight_request) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - request_time);
    return elapsed.count() > HEADERS_REQUEST_TIMEOUT_SEC;
}

void PeerHeaderState::markRequestSent(const std::string& from_hash) {
    has_inflight_request = true;
    request_time = std::chrono::steady_clock::now();
    requested_from_hash = from_hash;
    is_syncing = true;
}

void PeerHeaderState::clearRequest() {
    has_inflight_request = false;
    requested_from_hash.clear();
    is_syncing = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// HeaderChainCandidate Implementation
// ═══════════════════════════════════════════════════════════════════════════

void HeaderChainCandidate::calculateTotalWork() {
    total_work = 0;

    for (const auto& header : headers) {
        // Simplified work calculation: 2^256 / (target + 1)
        // In Bitcoin, target is derived from bits field
        // For now, use a simplified calculation based on bits

        // bits format: compact representation of target
        // Work roughly proportional to difficulty
        uint32_t bits = header.bits;

        // Simplified: higher bits = lower difficulty = less work
        // Actual Bitcoin formula is more complex
        if (bits > 0) {
            total_work += (uint64_t)(0xFFFFFFFFFFFFFFFFULL / bits);
        }
    }

    if (!headers.empty()) {
        start_height = headers.front().height;
        end_height = headers.back().height;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Checkpoints
// ═══════════════════════════════════════════════════════════════════════════

std::vector<Checkpoint> getCheckpoints() {
    return {
        // v7 restart genesis (April 17 2026).
        {0, "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f"},
        // Dinero v1 trust anchor. Verified across CN/LA/VA/MO/Dell/Mac on 2026-05-03.
        {13000, "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3"},
    };
}

// ═══════════════════════════════════════════════════════════════════════════
// MultiPeerHeadersSync Implementation
// ═══════════════════════════════════════════════════════════════════════════

MultiPeerHeadersSync::MultiPeerHeadersSync()
    : state_(SyncState::IDLE)
    , is_syncing_(false)
    , best_height_(0)
    , best_chain_work_(0)
    , max_parallel_peers_(MAX_PARALLEL_HEADER_PEERS)
    , max_headers_per_request_(MAX_HEADERS_PER_REQUEST)
    , request_timeout_(std::chrono::seconds(HEADERS_REQUEST_TIMEOUT_SEC))
    , total_headers_received_(0)
    , total_headers_validated_(0)
    , total_headers_rejected_(0)
    , total_requests_sent_(0)
    , total_timeouts_(0)
{
    checkpoints_ = getCheckpoints();

    dinero::g_logger.info("MultiPeerHeadersSync initialized");
    dinero::g_logger.info("  Max parallel peers: " + std::to_string(max_parallel_peers_));
    dinero::g_logger.info("  Max headers per request: " + std::to_string(max_headers_per_request_));
    dinero::g_logger.info("  Request timeout: " + std::to_string(request_timeout_.count()) + "s");
    dinero::g_logger.info("  Checkpoints loaded: " + std::to_string(checkpoints_.size()));
}

MultiPeerHeadersSync::~MultiPeerHeadersSync() {
    stopSync();
}

// ───────────────────────────────────────────────────────────────────────────
// Core Sync Operations
// ───────────────────────────────────────────────────────────────────────────

void MultiPeerHeadersSync::startSync(const std::vector<std::string>& peer_ids) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_syncing_) {
        dinero::g_logger.warning("Sync already in progress");
        return;
    }

    dinero::g_logger.info("Starting multi-peer header sync with " +
                         std::to_string(peer_ids.size()) + " peers");

    // Add all peers to tracking
    for (const auto& peer_id : peer_ids) {
        if (peer_states_.find(peer_id) == peer_states_.end()) {
            PeerHeaderState state;
            state.peer_id = peer_id;
            state.is_available = true;
            peer_states_[peer_id] = state;
        }
    }

    is_syncing_ = true;
    state_ = SyncState::REQUESTING_HEADERS;
    sync_start_time_ = std::chrono::steady_clock::now();

    // Immediately request headers from best peers
    requestNextHeaders();
}

void MultiPeerHeadersSync::stopSync() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_syncing_) {
        return;
    }

    dinero::g_logger.info("Stopping multi-peer header sync");

    is_syncing_ = false;
    state_ = SyncState::IDLE;

    // Clear all inflight requests
    for (auto& [peer_id, state] : peer_states_) {
        state.clearRequest();
    }
}

bool MultiPeerHeadersSync::isSyncing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_syncing_;
}

SyncState MultiPeerHeadersSync::getCurrentState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

// ───────────────────────────────────────────────────────────────────────────
// Header Processing
// ───────────────────────────────────────────────────────────────────────────

bool MultiPeerHeadersSync::processHeaders(const std::string& peer_id,
                                           const HeadersResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find peer state
    auto peer_it = peer_states_.find(peer_id);
    if (peer_it == peer_states_.end()) {
        dinero::g_logger.warning("Received headers from unknown peer: " + peer_id);
        return false;
    }

    PeerHeaderState& peer_state = peer_it->second;

    // Clear inflight request
    peer_state.clearRequest();

    // Check if we got any headers
    if (response.headers.empty()) {
        dinero::g_logger.debug("Peer " + peer_id + " sent 0 headers (up to date)");
        peer_state.is_syncing = false;
        return true;
    }

    dinero::g_logger.info("Processing " + std::to_string(response.headers.size()) +
                         " headers from peer " + peer_id);

    total_headers_received_ += response.headers.size();

    // Validate header chain
    const BlockHeader* prev_header = nullptr;
    if (!headers_chain_.empty()) {
        // Link to our current best chain
        const std::string& first_prev_hash = response.headers[0].prev_block_hash;
        auto it = hash_to_index_.find(first_prev_hash);
        if (it != hash_to_index_.end()) {
            prev_header = &headers_chain_[it->second];
        }
    }

    bool is_valid = validateHeaderChain(response.headers, prev_header);

    if (!is_valid) {
        dinero::g_logger.error("Invalid header chain from peer " + peer_id);
        peer_state.recordInvalidHeaders(response.headers.size());
        total_headers_rejected_ += response.headers.size();

        // Ban peer if score too low
        if (peer_state.header_score < MIN_PEER_HEADER_SCORE) {
            peer_state.is_available = false;
            dinero::g_logger.warning("Peer " + peer_id + " banned for low header score");
        }

        return false;
    }

    // Headers are valid - record success
    peer_state.recordValidHeaders(response.headers.size());
    total_headers_validated_ += response.headers.size();

    // Create chain candidate
    HeaderChainCandidate candidate;
    candidate.source_peer = peer_id;
    candidate.headers = response.headers;
    candidate.calculateTotalWork();
    candidate.is_validated = true;

    // Update peer's best known state
    const BlockHeader& last_header = response.headers.back();
    peer_state.best_known_hash = last_header.hash;
    peer_state.best_known_height = last_header.height;
    peer_state.best_known_work = candidate.total_work;

    // Compare with current best chain
    if (candidate.total_work > best_chain_work_ || headers_chain_.empty()) {
        dinero::g_logger.info("New best chain from peer " + peer_id +
                             " (work: " + std::to_string(candidate.total_work) +
                             " > " + std::to_string(best_chain_work_) + ")");

        // Integrate these headers into best chain
        integrateHeaders(response.headers);

        // Request more headers if we got the maximum
        if (response.headers.size() >= (size_t)max_headers_per_request_ && response.more_available) {
            dinero::g_logger.debug("Requesting more headers from peer " + peer_id);
            sendGetHeaders(peer_id, last_header.hash);
        }
    } else {
        dinero::g_logger.debug("Chain from peer " + peer_id +
                              " has less work than current best (ignoring)");
    }

    // If we didn't get the maximum, this peer is up to date
    if (response.headers.size() < (size_t)max_headers_per_request_) {
        peer_state.is_syncing = false;
        dinero::g_logger.info("Peer " + peer_id + " is up to date");
    }

    // Check if we need more headers from other peers
    if (needsMoreHeaders()) {
        requestNextHeaders();
    } else {
        // All peers are up to date
        dinero::g_logger.info("Header sync complete - all peers up to date");
        state_ = SyncState::SYNCED;
        is_syncing_ = false;
    }

    return true;
}

void MultiPeerHeadersSync::requestNextHeaders() {
    // Don't hold lock while calling sendGetHeaders (may call network code)
    std::vector<std::pair<std::string, std::string>> requests;  // (peer_id, from_hash)

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!is_syncing_) {
            return;
        }

        // Select best peers for requests
        std::vector<std::string> best_peers = selectBestPeers(max_parallel_peers_);

        if (best_peers.empty()) {
            dinero::g_logger.warning("No peers available for header sync");
            return;
        }

        // Prepare requests for each peer
        std::string from_hash = best_block_hash_;
        if (from_hash.empty() && !headers_chain_.empty()) {
            from_hash = headers_chain_.back().hash;
        }

        for (const auto& peer_id : best_peers) {
            auto& peer_state = peer_states_[peer_id];

            // Skip if already has inflight request
            if (peer_state.has_inflight_request) {
                continue;
            }

            // Mark request as sent
            peer_state.markRequestSent(from_hash);
            requests.push_back({peer_id, from_hash});
        }

        total_requests_sent_ += requests.size();
    }

    // Send requests (outside lock)
    for (const auto& [peer_id, from_hash] : requests) {
        sendGetHeaders(peer_id, from_hash);
    }

    if (!requests.empty()) {
        dinero::g_logger.info("Requested headers from " + std::to_string(requests.size()) + " peers");
    }
}

void MultiPeerHeadersSync::handleTimeouts() {
    std::vector<std::string> timed_out_peers;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find timed-out requests
        for (auto& [peer_id, state] : peer_states_) {
            if (state.hasTimedOut()) {
                timed_out_peers.push_back(peer_id);
            }
        }

        if (timed_out_peers.empty()) {
            return;
        }

        total_timeouts_ += timed_out_peers.size();

        // Update peer states
        for (const auto& peer_id : timed_out_peers) {
            auto& peer_state = peer_states_[peer_id];
            peer_state.recordTimeout();
            peer_state.clearRequest();

            // Ban peer if too many timeouts
            if (peer_state.timeouts >= 3) {
                peer_state.is_available = false;
                dinero::g_logger.warning("Peer " + peer_id + " banned for excessive timeouts");
            }
        }
    }

    dinero::g_logger.warning("Handled " + std::to_string(timed_out_peers.size()) + " timeout(s)");

    // Retry with different peers
    if (is_syncing_) {
        requestNextHeaders();
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Peer Management
// ───────────────────────────────────────────────────────────────────────────

void MultiPeerHeadersSync::addPeer(const std::string& peer_id, uint32_t best_height) {
    std::lock_guard<std::mutex> lock(mutex_);

    PeerHeaderState state;
    state.peer_id = peer_id;
    state.best_known_height = best_height;
    state.is_available = true;

    peer_states_[peer_id] = state;

    dinero::g_logger.debug("Added peer " + peer_id + " (height: " +
                          std::to_string(best_height) + ")");
}

void MultiPeerHeadersSync::removePeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    peer_states_.erase(peer_id);

    dinero::g_logger.debug("Removed peer " + peer_id);
}

std::vector<std::string> MultiPeerHeadersSync::selectBestPeers(int max_peers) {
    // Note: Caller must hold mutex_

    std::vector<std::pair<std::string, double>> peer_scores;

    // Score all available peers
    for (const auto& [peer_id, state] : peer_states_) {
        if (!state.is_available || state.has_inflight_request) {
            continue;
        }

        // Composite score based on:
        // - Header reputation score (60%)
        // - Best known height (40%)
        double score = (state.header_score * 0.6) +
                       (std::min((double)state.best_known_height, 100000.0) / 100000.0 * 40.0);

        peer_scores.push_back({peer_id, score});
    }

    // Sort by score (descending)
    std::sort(peer_scores.begin(), peer_scores.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Return top N
    std::vector<std::string> best_peers;
    for (size_t i = 0; i < peer_scores.size() && (int)i < max_peers; i++) {
        best_peers.push_back(peer_scores[i].first);
    }

    return best_peers;
}

// ───────────────────────────────────────────────────────────────────────────
// Chain Validation and Selection
// ───────────────────────────────────────────────────────────────────────────

bool MultiPeerHeadersSync::validateHeaderChain(const std::vector<BlockHeader>& headers,
                                                const BlockHeader* prev_header) {
    if (headers.empty()) {
        return true;
    }

    const BlockHeader* prev = prev_header;

    for (const auto& header : headers) {
        if (!validateHeader(header, prev)) {
            return false;
        }

        // Check checkpoint if present
        if (!verifyCheckpoint(header.height, header.hash)) {
            dinero::g_logger.error("Checkpoint mismatch at height " +
                                  std::to_string(header.height));
            return false;
        }

        prev = &header;
    }

    return true;
}

bool MultiPeerHeadersSync::validateHeader(const BlockHeader& header,
                                           const BlockHeader* prev_header) {
    // Check linkage
    if (prev_header != nullptr) {
        if (header.prev_block_hash != prev_header->hash) {
            dinero::g_logger.error("Header prev_block_hash doesn't match: " +
                                  header.prev_block_hash + " != " + prev_header->hash);
            return false;
        }

        // Check height increment
        if (header.height != prev_header->height + 1) {
            dinero::g_logger.error("Invalid height increment: " +
                                  std::to_string(header.height) + " != " +
                                  std::to_string(prev_header->height + 1));
            return false;
        }
    }

    // Check proof-of-work
    if (!checkProofOfWork(header)) {
        dinero::g_logger.error("Invalid proof-of-work for header at height " +
                              std::to_string(header.height));
        return false;
    }

    // Check timestamp is reasonable
    if (!isTimestampValid(header, prev_header)) {
        dinero::g_logger.error("Invalid timestamp for header at height " +
                              std::to_string(header.height));
        return false;
    }

    return true;
}

uint64_t MultiPeerHeadersSync::calculateChainWork(const std::vector<BlockHeader>& headers) {
    uint64_t total_work = 0;

    for (const auto& header : headers) {
        total_work += calculateHeaderWork(header);
    }

    return total_work;
}

size_t MultiPeerHeadersSync::selectBestChain(const std::vector<HeaderChainCandidate>& candidates) {
    if (candidates.empty()) {
        return 0;
    }

    size_t best_idx = 0;
    uint64_t best_work = candidates[0].total_work;

    for (size_t i = 1; i < candidates.size(); i++) {
        if (candidates[i].total_work > best_work) {
            best_work = candidates[i].total_work;
            best_idx = i;
        }
    }

    return best_idx;
}

bool MultiPeerHeadersSync::verifyCheckpoint(uint32_t height, const std::string& hash) {
    // Check if there's a checkpoint at this height
    for (const auto& checkpoint : checkpoints_) {
        if (checkpoint.height == height) {
            if (checkpoint.block_hash != hash) {
                dinero::g_logger.error("Checkpoint verification failed at height " +
                                      std::to_string(height) + ": expected " +
                                      checkpoint.block_hash + ", got " + hash);
                return false;
            }

            dinero::g_logger.info("Checkpoint verified at height " + std::to_string(height));
            return true;
        }
    }

    // No checkpoint at this height - that's fine
    return true;
}

// ───────────────────────────────────────────────────────────────────────────
// Status and Metrics
// ───────────────────────────────────────────────────────────────────────────

size_t MultiPeerHeadersSync::getActivePeerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto& [peer_id, state] : peer_states_) {
        if (state.is_available && state.is_syncing) {
            count++;
        }
    }

    return count;
}

std::unordered_map<std::string, PeerHeaderState> MultiPeerHeadersSync::getPeerStates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peer_states_;
}

din::Json MultiPeerHeadersSync::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    din::Json stats;
    stats["best_height"] = static_cast<int64_t>(best_height_);
    stats["is_syncing"] = is_syncing_;
    stats["peer_count"] = static_cast<int64_t>(peer_states_.size());

    return stats;
}

// ───────────────────────────────────────────────────────────────────────────
// Internal Helpers
// ───────────────────────────────────────────────────────────────────────────

void MultiPeerHeadersSync::sendGetHeaders(const std::string& peer_id,
                                           const std::string& from_hash) {
    if (!send_message_callback_) {
        dinero::g_logger.warning("Cannot send getheaders: no send callback set");
        return;
    }

    // Get Bitcoin-style header locator (exponential backoff)
    std::vector<std::string> locator = getHeaderLocator();

    // If from_hash is provided and not in locator, add it at the beginning
    if (!from_hash.empty() && (locator.empty() || locator.front() != from_hash)) {
        locator.insert(locator.begin(), from_hash);
    }

    // Empty hash_stop means "give me everything after locator"
    std::string hash_stop;

    dinero::g_logger.debug("Sending getheaders to peer " + peer_id +
                          " (locator size: " + std::to_string(locator.size()) + ")");

    // Call the callback to actually send the P2P message
    bool success = send_message_callback_(peer_id, locator, hash_stop);

    if (!success) {
        dinero::g_logger.warning("Failed to send getheaders to peer " + peer_id);
    }
}

bool MultiPeerHeadersSync::needsMoreHeaders() const {
    // Note: Caller must hold mutex_

    // Check if any peer is still syncing
    for (const auto& [peer_id, state] : peer_states_) {
        if (state.is_syncing || state.has_inflight_request) {
            return true;
        }

        // Check if peer knows about higher blocks
        if (state.best_known_height > best_height_) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> MultiPeerHeadersSync::getHeaderLocator() const {
    // Note: Caller must hold mutex_

    // Bitcoin-style locator: exponential backoff
    // Most recent blocks, then skip 1, 2, 4, 8, 16... back to genesis

    std::vector<std::string> locator;

    if (headers_chain_.empty()) {
        return locator;
    }

    // Add recent headers
    size_t step = 1;
    int64_t idx = headers_chain_.size() - 1;

    while (idx >= 0) {
        locator.push_back(headers_chain_[idx].hash);

        // Skip backwards with exponential step
        idx -= step;

        // Double the step after first 10 blocks
        if (locator.size() > 10) {
            step *= 2;
        }
    }

    return locator;
}

void MultiPeerHeadersSync::integrateHeaders(const std::vector<BlockHeader>& headers) {
    // Note: Caller must hold mutex_

    for (const auto& header : headers) {
        // Check if we already have this header
        if (hash_to_index_.find(header.hash) != hash_to_index_.end()) {
            continue;  // Already have it
        }

        // Add to chain
        size_t index = headers_chain_.size();
        headers_chain_.push_back(header);
        hash_to_index_[header.hash] = index;

        // Update best if this is higher
        if (header.height > best_height_) {
            best_height_ = header.height;
            best_block_hash_ = header.hash;
            best_chain_work_ += calculateHeaderWork(header);

            dinero::g_logger.info("New best header: height=" +
                                 std::to_string(best_height_) +
                                 " hash=" + best_block_hash_.substr(0, 16) + "...");
        }
    }
}

uint64_t MultiPeerHeadersSync::calculateHeaderWork(const BlockHeader& header) {
    // Simplified work calculation based on difficulty (bits field)
    // Real Bitcoin: work = 2^256 / (target + 1)

    if (header.bits == 0) {
        return 0;
    }

    // Simplified: inverse of bits as approximate work
    // Higher bits = easier target = less work
    return 0xFFFFFFFFFFFFFFFFULL / header.bits;
}

bool MultiPeerHeadersSync::isTimestampValid(const BlockHeader& header,
                                             const BlockHeader* prev_header) {
    // Check timestamp is not too far in the future
    auto now = std::chrono::system_clock::now();
    auto now_timestamp = std::chrono::system_clock::to_time_t(now);
    constexpr uint32_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours

    if (header.timestamp > now_timestamp + MAX_FUTURE_BLOCK_TIME) {
        return false;
    }

    // Check timestamp is after previous block
    if (prev_header != nullptr) {
        if (header.timestamp < prev_header->timestamp) {
            return false;
        }
    }

    return true;
}

bool MultiPeerHeadersSync::checkProofOfWork(const BlockHeader& header) {
    // Phase D.4: Use consensus PoW verification (Bitcoin Core-compatible)
    // This performs:
    // 1. Convert bits to 256-bit target
    // 2. Check hash <= target
    //
    // Note: p2p::BlockHeader has pre-computed hash field, use it directly

    // Convert bits to target
    auto target = dinero::consensus::BitsToTarget(header.bits);
    if (target.empty()) {
        return false;  // Invalid bits
    }

    // Convert header.hash (hex string) to bytes
    if (header.hash.size() != 64) {
        return false;  // Invalid hash length
    }

    std::vector<uint8_t> hash_bytes(32);
    for (size_t i = 0; i < 32; ++i) {
        // Hash is stored big-endian in hex, convert to bytes
        unsigned int byte;
        std::sscanf(header.hash.c_str() + (i * 2), "%02x", &byte);
        hash_bytes[i] = static_cast<uint8_t>(byte);
    }

    return dinero::consensus::HashMeetsTarget(hash_bytes, target);
}

} // namespace p2p
} // namespace dinero
