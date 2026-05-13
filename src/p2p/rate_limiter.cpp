// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/rate_limiter.h"
#include "p2p/peer_scoring.h"
#include "common/logger.h"
#include <chrono>
#include <algorithm>

namespace dinero {

RateLimiter::RateLimiter(
    const RateLimiterConfig& config,
    std::shared_ptr<p2p::PeerScoringManager> scoring_manager)
    : config_(config), scoring_manager_(scoring_manager) {

    g_logger.info("RateLimiter initialized: "
                 "max_tokens=" + std::to_string(config_.max_tokens) +
                 ", refill_rate=" + std::to_string(config_.refill_rate) +
                 ", ban_threshold=" + std::to_string(config_.ban_threshold) +
                 ", enabled=" + std::string(config_.enabled ? "true" : "false"));
}

bool RateLimiter::allowMessage(peer_id_t peer_id, uint32_t cost) {
    if (!config_.enabled) {
        return true;  // Rate limiting disabled
    }

    std::lock_guard<std::mutex> lock(buckets_mutex_);

    // Get or create bucket for this peer
    TokenBucket& bucket = getOrCreateBucket(peer_id);

    // Refill bucket based on elapsed time
    int64_t current_time = getCurrentTime();
    refillBucket(bucket, current_time);

    // Check if peer has enough tokens
    if (bucket.tokens >= static_cast<double>(cost)) {
        // Consume tokens
        bucket.tokens -= static_cast<double>(cost);
        bucket.messages_allowed++;

        // Update stats
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.total_messages_allowed++;
        }

        return true;
    }

    // Rate limit exceeded
    bucket.violations++;
    bucket.messages_rejected++;

    // Update stats
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_messages_rejected++;
        stats_.total_violations++;
    }

    g_logger.debug("Rate limit exceeded for peer " + peer_id +
                  " (cost=" + std::to_string(cost) +
                  ", available=" + std::to_string(bucket.tokens) +
                  ", violations=" + std::to_string(bucket.violations) + ")");

    // Check if ban threshold exceeded
    if (bucket.violations >= config_.ban_threshold && scoring_manager_) {
        g_logger.warning("Peer " + peer_id +
                        " exceeded rate limit ban threshold (" +
                        std::to_string(config_.ban_threshold) +
                        " violations), adding misbehavior score");

        // Add misbehavior score (will be handled by PeerScoringManager)
        scoring_manager_->addMisbehavior(peer_id, p2p::MisbehaviorType::EXCESSIVE_REQUESTS);

        // Reset violations counter (to avoid repeated scoring)
        bucket.violations = 0;
    }

    return false;
}

void RateLimiter::refillBuckets() {
    if (!config_.enabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(buckets_mutex_);
    int64_t current_time = getCurrentTime();

    for (auto& [peer_id, bucket] : peer_buckets_) {
        refillBucket(bucket, current_time);
    }
}

void RateLimiter::removePeer(peer_id_t peer_id) {
    std::lock_guard<std::mutex> lock(buckets_mutex_);

    auto it = peer_buckets_.find(peer_id);
    if (it != peer_buckets_.end()) {
        g_logger.debug("Removed rate limiter bucket for peer " + peer_id);
        peer_buckets_.erase(it);

        // Update stats
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        if (stats_.total_peers > 0) {
            stats_.total_peers--;
        }
    }
}

double RateLimiter::getTokens(peer_id_t peer_id) const {
    std::lock_guard<std::mutex> lock(buckets_mutex_);

    auto it = peer_buckets_.find(peer_id);
    if (it != peer_buckets_.end()) {
        return it->second.tokens;
    }

    return 0.0;
}

uint32_t RateLimiter::getViolations(peer_id_t peer_id) const {
    std::lock_guard<std::mutex> lock(buckets_mutex_);

    auto it = peer_buckets_.find(peer_id);
    if (it != peer_buckets_.end()) {
        return it->second.violations;
    }

    return 0;
}

void RateLimiter::reset() {
    {
        std::lock_guard<std::mutex> lock(buckets_mutex_);
        peer_buckets_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = Stats();
    }

    g_logger.info("RateLimiter reset complete");
}

RateLimiter::Stats RateLimiter::getStats() const {
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    Stats result = stats_;

    // Update total_peers count
    {
        std::lock_guard<std::mutex> buckets_lock(buckets_mutex_);
        result.total_peers = static_cast<uint32_t>(peer_buckets_.size());
    }

    return result;
}

RateLimiter::TokenBucket& RateLimiter::getOrCreateBucket(peer_id_t peer_id) {
    // Must be called with buckets_mutex_ held

    auto it = peer_buckets_.find(peer_id);
    if (it != peer_buckets_.end()) {
        return it->second;
    }

    // Create new bucket with full capacity
    TokenBucket new_bucket;
    new_bucket.tokens = static_cast<double>(config_.max_tokens);
    new_bucket.last_refill_time = getCurrentTime();
    new_bucket.violations = 0;
    new_bucket.messages_allowed = 0;
    new_bucket.messages_rejected = 0;

    peer_buckets_[peer_id] = new_bucket;

    g_logger.debug("Created rate limiter bucket for peer " + peer_id +
                  " (initial tokens=" + std::to_string(new_bucket.tokens) + ")");

    // Update stats
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_peers++;
    }

    return peer_buckets_[peer_id];
}

void RateLimiter::refillBucket(TokenBucket& bucket, int64_t current_time) {
    // Calculate elapsed time since last refill
    int64_t elapsed_seconds = current_time - bucket.last_refill_time;

    if (elapsed_seconds <= 0) {
        return;  // No time elapsed or clock went backwards
    }

    // Calculate tokens to add
    double tokens_to_add = config_.refill_rate * static_cast<double>(elapsed_seconds);

    // Add tokens (capped at max_tokens)
    bucket.tokens = std::min(
        bucket.tokens + tokens_to_add,
        static_cast<double>(config_.max_tokens)
    );

    // Update last refill time
    bucket.last_refill_time = current_time;
}

int64_t RateLimiter::getCurrentTime() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace dinero
