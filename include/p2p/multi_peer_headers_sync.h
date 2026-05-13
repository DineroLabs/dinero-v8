#pragma once

#include "p2p/headers_first_sync.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <chrono>
#include <mutex>

namespace dinero {
namespace p2p {

/**
 * @file multi_peer_headers_sync.h
 * @brief MAINNET BLOCKER: Multi-Peer Header Sync Implementation
 *
 * Bitcoin-compatible headers-first sync with parallel peer support:
 * - Request headers from 4-8 peers simultaneously
 * - Track per-peer header state and best chain
 * - Conflict resolution using most cumulative work
 * - Peer reputation tracking for header quality
 * - Checkpoint verification against hardcoded checkpoints
 *
 * This implements Bitcoin Core's headers-first sync strategy from:
 * - https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp
 * - ProcessHeadersMessage(), SendGetHeaders(), etc.
 *
 * Estimated: 3-4 days (Critical mainnet blocker)
 */

// ═══════════════════════════════════════════════════════════════════════════
// Configuration Constants
// ═══════════════════════════════════════════════════════════════════════════

namespace multi_peer_sync {

static constexpr int MAX_PARALLEL_HEADER_PEERS = 8;        // Request from 8 peers simultaneously
static constexpr int MAX_HEADERS_PER_REQUEST = 2000;       // Bitcoin protocol limit
static constexpr int HEADERS_REQUEST_TIMEOUT_SEC = 30;     // 30s timeout for header responses
static constexpr int MAX_HEADER_REQUESTS_IN_FLIGHT = 16;   // Max concurrent header requests
static constexpr int MIN_PEER_HEADER_SCORE = 50;           // Minimum score to request headers from peer

// Peer reputation scoring
static constexpr double VALID_HEADER_REWARD = 1.0;         // +1 for valid header
static constexpr double INVALID_HEADER_PENALTY = -10.0;    // -10 for invalid header
static constexpr double TIMEOUT_PENALTY = -5.0;            // -5 for timeout

} // namespace multi_peer_sync

// ═══════════════════════════════════════════════════════════════════════════
// Per-Peer Header State Tracking
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Tracks header sync state for a single peer
 *
 * Maintains:
 * - Best header this peer knows about (pindexBestKnownBlock in Bitcoin Core)
 * - In-flight header requests to this peer
 * - Peer reputation score for header quality
 * - Timeout tracking
 */
struct PeerHeaderState {
    std::string peer_id;                                    // Peer identifier

    // Best known state
    std::string best_known_hash;                            // Best header hash peer knows
    uint32_t best_known_height{0};                          // Best height peer knows
    uint64_t best_known_work{0};                            // Cumulative work of best chain

    // Request tracking
    bool has_inflight_request{false};                       // Currently waiting for headers
    std::chrono::steady_clock::time_point request_time;     // When we sent the request
    std::string requested_from_hash;                        // Hash we requested from

    // Reputation and scoring
    double header_score{100.0};                             // Reputation score (0-100)
    uint32_t valid_headers_received{0};                     // Count of valid headers
    uint32_t invalid_headers_received{0};                   // Count of invalid headers
    uint32_t timeouts{0};                                   // Count of timeouts

    // State
    bool is_available{true};                                // Can we request from this peer?
    bool is_syncing{false};                                 // Active sync with this peer
    std::chrono::steady_clock::time_point last_activity;    // Last successful response

    /**
     * @brief Update score after receiving valid headers
     */
    void recordValidHeaders(uint32_t count);

    /**
     * @brief Update score after receiving invalid headers
     */
    void recordInvalidHeaders(uint32_t count);

    /**
     * @brief Update score after timeout
     */
    void recordTimeout();

    /**
     * @brief Check if peer's inflight request has timed out
     */
    bool hasTimedOut() const;

    /**
     * @brief Mark request as sent
     */
    void markRequestSent(const std::string& from_hash);

    /**
     * @brief Clear inflight request
     */
    void clearRequest();
};

// ═══════════════════════════════════════════════════════════════════════════
// Header Chain Candidate
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Represents a candidate header chain from a peer
 *
 * When multiple peers provide different chains, we track each as a candidate
 * and select the one with most cumulative work.
 */
struct HeaderChainCandidate {
    std::string source_peer;                                // Peer that provided this chain
    std::vector<BlockHeader> headers;                       // Headers in this chain
    uint64_t total_work{0};                                 // Cumulative proof-of-work
    uint32_t start_height{0};                               // First header height
    uint32_t end_height{0};                                 // Last header height
    bool is_validated{false};                               // Passed validation

    /**
     * @brief Calculate cumulative work for this chain
     */
    void calculateTotalWork();

    /**
     * @brief Check if this chain has more work than another
     */
    bool hasMoreWorkThan(const HeaderChainCandidate& other) const {
        return total_work > other.total_work;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Hardcoded Checkpoints
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Hardcoded checkpoint for chain verification
 *
 * Checkpoints ensure we're on the correct chain and prevent long reorgs.
 * These are hardcoded into the software at release time.
 */
struct Checkpoint {
    uint32_t height;
    std::string block_hash;
};

/**
 * @brief Get list of hardcoded checkpoints
 *
 * TODO: Update these with actual mainnet checkpoints at launch
 */
std::vector<Checkpoint> getCheckpoints();

// ═══════════════════════════════════════════════════════════════════════════
// Multi-Peer Headers Sync Manager
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Main coordinator for multi-peer header synchronization
 *
 * Implements Bitcoin Core-compatible headers-first sync with parallel peers:
 *
 * Key Responsibilities:
 * 1. Maintain per-peer header state tracking
 * 2. Request headers from multiple peers in parallel
 * 3. Validate header chains (PoW, linkage, timestamps)
 * 4. Resolve conflicts using most-work selection
 * 5. Track peer reputation for header quality
 * 6. Verify against hardcoded checkpoints
 * 7. Coordinate with block downloader after headers validated
 *
 * Integration with HeadersFirstSync:
 * - Enhances single-peer sync with multi-peer capabilities
 * - Can be used as drop-in replacement or wrapper
 * - Shares same BlockHeader and HeadersResponse structures
 */
class MultiPeerHeadersSync {
public:
    // Callback type for sending getheaders messages to peers
    using SendMessageCallback = std::function<bool(const std::string& peer_id,
                                                     const std::vector<std::string>& locator_hashes,
                                                     const std::string& hash_stop)>;

    MultiPeerHeadersSync();
    ~MultiPeerHeadersSync();

    /**
     * @brief Set callback for sending getheaders messages
     *
     * Active P2P routing must provide this callback to enable actual message sending
     */
    void setSendMessageCallback(SendMessageCallback callback) {
        send_message_callback_ = std::move(callback);
    }

    // ───────────────────────────────────────────────────────────────────────
    // Core Sync Operations
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Start parallel header sync with multiple peers
     *
     * @param peer_ids List of peer IDs to sync from
     */
    void startSync(const std::vector<std::string>& peer_ids);

    /**
     * @brief Stop ongoing header sync
     */
    void stopSync();

    /**
     * @brief Check if currently syncing
     */
    bool isSyncing() const;

    /**
     * @brief Get current sync state
     */
    SyncState getCurrentState() const;

    // ───────────────────────────────────────────────────────────────────────
    // Header Processing
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Process headers received from a peer
     *
     * Main header processing logic:
     * 1. Validate header chain (PoW, linkage, timestamps)
     * 2. Calculate cumulative work
     * 3. Compare with current best chain
     * 4. If more work, switch to this chain
     * 5. Update peer state and reputation
     * 6. Request more headers if needed
     *
     * @param peer_id Peer that sent these headers
     * @param response Headers response from peer
     * @return true if headers were valid and accepted
     */
    bool processHeaders(const std::string& peer_id, const HeadersResponse& response);

    /**
     * @brief Request next batch of headers from best peers
     *
     * Selects N best peers and requests headers from each in parallel.
     * Uses peer reputation scores to prioritize reliable peers.
     */
    void requestNextHeaders();

    /**
     * @brief Handle timeout for inflight header requests
     *
     * Check all peers with inflight requests for timeouts.
     * Update peer scores and retry with different peers if needed.
     */
    void handleTimeouts();

    // ───────────────────────────────────────────────────────────────────────
    // Peer Management
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Add peer to sync pool
     *
     * @param peer_id Peer identifier
     * @param best_height Peer's reported best height (from version message)
     */
    void addPeer(const std::string& peer_id, uint32_t best_height);

    /**
     * @brief Remove peer from sync pool
     *
     * @param peer_id Peer that disconnected
     */
    void removePeer(const std::string& peer_id);

    /**
     * @brief Get best N peers for header requests
     *
     * Selects peers based on:
     * - Header reputation score
     * - Best known height
     * - Not currently in-flight
     *
     * @param max_peers Maximum number of peers to return
     * @return Vector of best peer IDs
     */
    std::vector<std::string> selectBestPeers(int max_peers);

    // ───────────────────────────────────────────────────────────────────────
    // Chain Validation and Selection
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Validate a chain of headers
     *
     * Checks:
     * - Each header links to previous (prev_block_hash matches)
     * - Proof-of-work is valid for each header
     * - Timestamps are reasonable (not too far in future)
     * - Difficulty adjustments are correct
     * - Checkpoints match if present
     *
     * @param headers Chain of headers to validate
     * @param prev_header Previous header to link to (nullptr if genesis)
     * @return true if entire chain is valid
     */
    bool validateHeaderChain(const std::vector<BlockHeader>& headers,
                             const BlockHeader* prev_header);

    /**
     * @brief Validate a single header
     *
     * @param header Header to validate
     * @param prev_header Previous header (for linkage)
     * @return true if header is valid
     */
    bool validateHeader(const BlockHeader& header, const BlockHeader* prev_header);

    /**
     * @brief Calculate cumulative work for a chain
     *
     * Work = sum of 2^256 / (target + 1) for each header
     *
     * @param headers Chain of headers
     * @return Total cumulative work
     */
    uint64_t calculateChainWork(const std::vector<BlockHeader>& headers);

    /**
     * @brief Select best chain from multiple candidates
     *
     * Uses most cumulative work as tiebreaker.
     * In Bitcoin, the chain with most work wins.
     *
     * @param candidates Vector of chain candidates
     * @return Index of best candidate
     */
    size_t selectBestChain(const std::vector<HeaderChainCandidate>& candidates);

    /**
     * @brief Verify checkpoint at given height
     *
     * @param height Block height
     * @param hash Block hash
     * @return true if matches checkpoint or no checkpoint at this height
     */
    bool verifyCheckpoint(uint32_t height, const std::string& hash);

    // ───────────────────────────────────────────────────────────────────────
    // Status and Metrics
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get current best header height
     */
    uint32_t getBestHeight() const { return best_height_; }

    /**
     * @brief Get current best header hash
     */
    std::string getBestBlockHash() const { return best_block_hash_; }

    /**
     * @brief Get total headers downloaded
     */
    uint32_t getHeadersCount() const { return headers_chain_.size(); }

    /**
     * @brief Get number of active sync peers
     */
    size_t getActivePeerCount() const;

    /**
     * @brief Get per-peer header states (for monitoring/debugging)
     */
    std::unordered_map<std::string, PeerHeaderState> getPeerStates() const;

    /**
     * @brief Get sync statistics for RPC/monitoring
     */
    din::Json getStats() const;

private:
    // ───────────────────────────────────────────────────────────────────────
    // Internal Helpers
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Send getheaders message to peer
     */
    void sendGetHeaders(const std::string& peer_id, const std::string& from_hash);

    /**
     * @brief Check if we should request more headers
     */
    bool needsMoreHeaders() const;

    /**
     * @brief Get locator for getheaders request
     *
     * Bitcoin uses exponential backoff: most recent headers, then
     * skip 1, 2, 4, 8, 16... blocks back to genesis.
     */
    std::vector<std::string> getHeaderLocator() const;

    /**
     * @brief Integrate new headers into best chain
     */
    void integrateHeaders(const std::vector<BlockHeader>& headers);

    /**
     * @brief Calculate work for a single header
     */
    uint64_t calculateHeaderWork(const BlockHeader& header);

    /**
     * @brief Check if header timestamp is reasonable
     */
    bool isTimestampValid(const BlockHeader& header, const BlockHeader* prev_header);

    /**
     * @brief Verify proof-of-work for header
     */
    bool checkProofOfWork(const BlockHeader& header);

    // ───────────────────────────────────────────────────────────────────────
    // Member Variables
    // ───────────────────────────────────────────────────────────────────────

    mutable std::mutex mutex_;

    // Sync state
    SyncState state_;
    bool is_syncing_;
    std::chrono::steady_clock::time_point sync_start_time_;

    // Best chain
    std::vector<BlockHeader> headers_chain_;                // Main header chain
    std::unordered_map<std::string, size_t> hash_to_index_; // Hash -> index lookup
    uint32_t best_height_;
    std::string best_block_hash_;
    uint64_t best_chain_work_;

    // Per-peer state tracking
    std::unordered_map<std::string, PeerHeaderState> peer_states_;

    // Chain candidates (for conflict resolution)
    std::vector<HeaderChainCandidate> chain_candidates_;

    // Checkpoints
    std::vector<Checkpoint> checkpoints_;

    // Configuration
    int max_parallel_peers_;
    int max_headers_per_request_;
    std::chrono::seconds request_timeout_;

    // Metrics
    uint32_t total_headers_received_;
    uint32_t total_headers_validated_;
    uint32_t total_headers_rejected_;
    uint32_t total_requests_sent_;
    uint32_t total_timeouts_;

    // Callback for sending messages to peers
    SendMessageCallback send_message_callback_;
};

} // namespace p2p
} // namespace dinero
