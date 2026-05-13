#pragma once

#include <chrono>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace dinero {
namespace ws {

/**
 * Token bucket rate limiter for WebSocket connections
 *
 * Each connection gets a bucket with:
 * - Fixed capacity (max burst size)
 * - Constant refill rate (tokens per second)
 *
 * Usage:
 *   RateLimiter limiter;
 *   if (limiter.AllowMessage(fd)) {
 *       // Process message
 *   } else {
 *       // Reject/drop message
 *   }
 */
class RateLimiter {
public:
    struct Config {
        uint64_t capacity = 100;        // Max tokens (burst limit)
        uint64_t refill_rate = 10;      // Tokens per second
        uint64_t cost_per_message = 1;  // Tokens consumed per message

        static Config Default() { return Config{}; }
        static Config Strict() { return Config{50, 5, 1}; }
        static Config Lenient() { return Config{200, 20, 1}; }
    };

    explicit RateLimiter(const Config& config = Config::Default());
    ~RateLimiter() = default;

    // Check if connection can send a message (consumes tokens if allowed)
    bool AllowMessage(int fd);

    // Refill tokens for a specific connection (manual refill)
    void Refill(int fd);

    // Remove connection from rate limiter (cleanup)
    void RemoveConnection(int fd);

    // Get current token count for connection (for debugging)
    uint64_t GetTokens(int fd) const;

    // Get stats for monitoring
    struct Stats {
        size_t active_connections = 0;
        uint64_t total_allowed = 0;
        uint64_t total_rejected = 0;
    };
    Stats GetStats() const;

private:
    struct TokenBucket {
        uint64_t tokens;
        uint64_t capacity;
        uint64_t refill_rate;
        std::chrono::steady_clock::time_point last_refill;

        TokenBucket(uint64_t cap, uint64_t rate)
            : tokens(cap),
              capacity(cap),
              refill_rate(rate),
              last_refill(std::chrono::steady_clock::now()) {}

        bool Consume(uint64_t count = 1) {
            Refill();
            if (tokens >= count) {
                tokens -= count;
                return true;
            }
            return false;
        }

        void Refill() {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_refill).count();

            if (elapsed > 0) {
                tokens = std::min(capacity, tokens + (elapsed * refill_rate));
                last_refill = now;
            }
        }
    };

    mutable std::mutex mutex_;
    std::unordered_map<int, TokenBucket> buckets_;
    Config config_;

    // Stats (atomic would be better but keeping simple for now)
    mutable uint64_t total_allowed_ = 0;
    mutable uint64_t total_rejected_ = 0;
};

} // namespace ws
} // namespace dinero
