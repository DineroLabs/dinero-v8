// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "network/types.h"
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace dinero {

// Forward declaration
namespace p2p {
class PeerScoringManager;
enum class MisbehaviorType;
}

/**
 * Rate limiter configuration
 * Token bucket algorithm parameters
 */
struct RateLimiterConfig {
    uint32_t max_tokens{100};           // Maximum bucket capacity (tokens)
    double refill_rate{10.0};           // Tokens per second refill rate
    uint32_t ban_threshold{5};          // Violations before triggering ban
    bool enabled{true};                 // Enable/disable rate limiting
};

/**
 * Per-message cost configuration
 * Different message types consume different amounts of tokens
 */
struct MessageCost {
    static constexpr uint32_t PING = 1;
    static constexpr uint32_t PONG = 1;
    static constexpr uint32_t VERSION = 5;
    static constexpr uint32_t VERACK = 5;
    static constexpr uint32_t ADDR = 10;
    static constexpr uint32_t INV = 5;
    static constexpr uint32_t GETDATA = 10;
    static constexpr uint32_t BLOCK = 50;
    static constexpr uint32_t TX = 20;
    static constexpr uint32_t HEADERS = 30;
    static constexpr uint32_t GETHEADERS = 10;
    static constexpr uint32_t GETBLOCKS = 10;

    // Phase 7.4.4: Utreexo proof serving costs
    static constexpr uint32_t GETUTREEXOPROOF = 15;   // Request proofs (expensive)
    static constexpr uint32_t UTREEXOPROOF = 40;      // Proof response (similar to BLOCK)
    static constexpr uint32_t GETUTREEXOHDRS = 10;    // Request headers (like GETHEADERS)
    static constexpr uint32_t UTREEXOHDRS = 25;       // Headers response (less than BLOCK)

    static constexpr uint32_t DEFAULT = 5;
};

/**
 * RateLimiter — Token bucket rate limiting for DoS protection
 *
 * AUTHORITY: Message rate enforcement (when to accept/reject messages)
 * DOES NOT: Validate message content, manage connections, track scores
 *
 * Responsibilities:
 * 1. Maintain per-peer token buckets
 * 2. Refill tokens at configured rate
 * 3. Consume tokens for each message
 * 4. Track rate limit violations
 * 5. Integrate with PeerScoringManager for bans
 *
 * Token Bucket Algorithm:
 * - Each peer starts with max_tokens in their bucket
 * - Tokens refill at refill_rate per second (e.g., 10 tokens/sec)
 * - Each message consumes tokens based on cost (e.g., BLOCK = 50 tokens)
 * - If bucket has insufficient tokens, message is rejected
 * - Repeated violations trigger misbehavior scoring
 *
 * Phase N.0 Inventory Findings:
 * - No message rate limiting exists (DoS vulnerability)
 * - Malicious peer can flood with INV/GETDATA messages
 * - Need token bucket to enforce fair resource usage
 *
 * Bitcoin Core Approach:
 * - Uses token bucket for bandwidth limiting
 * - Different costs for different message types
 * - Violation threshold before disconnection
 */
class RateLimiter {
public:
    /**
     * Constructor
     * @param config Rate limiter configuration
     * @param scoring_manager Optional peer scoring integration
     */
    explicit RateLimiter(
        const RateLimiterConfig& config,
        std::shared_ptr<p2p::PeerScoringManager> scoring_manager = nullptr
    );

    /**
     * Check if peer is allowed to send a message
     * Consumes tokens if allowed
     *
     * @param peer_id Peer identifier
     * @param cost Token cost of the message (default: MessageCost::DEFAULT)
     * @return true if message allowed, false if rate-limited
     */
    bool allowMessage(peer_id_t peer_id, uint32_t cost = MessageCost::DEFAULT);

    /**
     * Refill all peer token buckets
     * Should be called periodically (e.g., every 100ms)
     */
    void refillBuckets();

    /**
     * Remove peer from rate limiter
     * Called when peer disconnects
     *
     * @param peer_id Peer identifier
     */
    void removePeer(peer_id_t peer_id);

    /**
     * Get current token count for peer
     * For debugging/monitoring
     *
     * @param peer_id Peer identifier
     * @return Current tokens available (0 if peer unknown)
     */
    double getTokens(peer_id_t peer_id) const;

    /**
     * Get violation count for peer
     *
     * @param peer_id Peer identifier
     * @return Number of rate limit violations
     */
    uint32_t getViolations(peer_id_t peer_id) const;

    /**
     * Reset all buckets (for testing)
     */
    void reset();

    /**
     * Get statistics
     */
    struct Stats {
        uint32_t total_peers{0};
        uint32_t total_violations{0};
        uint32_t total_messages_allowed{0};
        uint32_t total_messages_rejected{0};
    };
    Stats getStats() const;

private:
    /**
     * Per-peer token bucket state
     */
    struct TokenBucket {
        double tokens{0.0};              // Current tokens available
        int64_t last_refill_time{0};     // Last refill timestamp (unix seconds)
        uint32_t violations{0};          // Number of rate limit violations
        uint64_t messages_allowed{0};    // Total messages allowed
        uint64_t messages_rejected{0};   // Total messages rejected
    };

    // Configuration
    RateLimiterConfig config_;
    std::shared_ptr<p2p::PeerScoringManager> scoring_manager_;

    // Per-peer buckets (thread-safe)
    mutable std::mutex buckets_mutex_;
    std::unordered_map<peer_id_t, TokenBucket> peer_buckets_;

    // Global statistics
    mutable std::mutex stats_mutex_;
    Stats stats_;

    /**
     * Get or create token bucket for peer
     * Must be called with buckets_mutex_ held
     */
    TokenBucket& getOrCreateBucket(peer_id_t peer_id);

    /**
     * Refill a single bucket based on elapsed time
     */
    void refillBucket(TokenBucket& bucket, int64_t current_time);

    /**
     * Get current Unix timestamp in seconds
     */
    int64_t getCurrentTime() const;
};

} // namespace dinero
