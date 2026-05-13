#include "ws/ws_rate_limiter.h"
#include <algorithm>

namespace dinero {
namespace ws {

RateLimiter::RateLimiter(const Config& config)
    : config_(config) {
}

bool RateLimiter::AllowMessage(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get or create bucket for this connection
    auto it = buckets_.find(fd);
    if (it == buckets_.end()) {
        // Create new bucket with full capacity
        it = buckets_.emplace(fd, TokenBucket(config_.capacity, config_.refill_rate)).first;
    }

    // Try to consume tokens
    bool allowed = it->second.Consume(config_.cost_per_message);

    if (allowed) {
        total_allowed_++;
    } else {
        total_rejected_++;
    }

    return allowed;
}

void RateLimiter::Refill(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(fd);
    if (it != buckets_.end()) {
        it->second.Refill();
    }
}

void RateLimiter::RemoveConnection(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    buckets_.erase(fd);
}

uint64_t RateLimiter::GetTokens(int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(fd);
    if (it != buckets_.end()) {
        // Const-cast hack to call non-const Refill (should be const)
        const_cast<TokenBucket&>(it->second).Refill();
        return it->second.tokens;
    }

    return 0;
}

RateLimiter::Stats RateLimiter::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.active_connections = buckets_.size();
    stats.total_allowed = total_allowed_;
    stats.total_rejected = total_rejected_;

    return stats;
}

} // namespace ws
} // namespace dinero
