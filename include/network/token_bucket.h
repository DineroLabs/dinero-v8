// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Token-bucket rate limiter — backs the relay circuit bandwidth caps.
//
// A bucket holds up to `capacity` bytes of tokens and refills at `rate`
// bytes/sec. TryConsume(n) first refills for the elapsed wall time, then
// consumes n tokens if enough are available. Burst size is bounded by
// `capacity`; sustained throughput is bounded by `rate`.
//
// NOT thread-safe — the caller serializes access (per-circuit buckets
// and the global bucket are both touched only under circuits_mutex_).
// A default-constructed (zero-capacity) bucket is "disabled" and always
// admits — so a misconfiguration fails open, never silently blackholes.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace dinero::network {

class TokenBucket {
public:
    TokenBucket() = default;
    TokenBucket(double rate_bytes_per_sec, double capacity_bytes)
        : rate_(rate_bytes_per_sec),
          capacity_(capacity_bytes),
          tokens_(capacity_bytes) {}

    // Refill for elapsed time, then consume `bytes`. Returns true and
    // deducts the tokens on success; returns false and deducts nothing
    // when the bucket is short. A zero-capacity bucket always returns
    // true (unconfigured => no limit).
    bool TryConsume(std::size_t bytes,
                    std::chrono::steady_clock::time_point now) {
        if (capacity_ <= 0.0) return true;
        if (!initialized_) {
            last_refill_ = now;
            initialized_ = true;
        }
        const double elapsed =
            std::chrono::duration<double>(now - last_refill_).count();
        if (elapsed > 0.0) {
            tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
            last_refill_ = now;
        }
        const double need = static_cast<double>(bytes);
        if (tokens_ < need) return false;
        tokens_ -= need;
        return true;
    }

private:
    double rate_ = 0.0;      // bytes per second
    double capacity_ = 0.0;  // max tokens in bytes; 0 => disabled
    double tokens_ = 0.0;
    std::chrono::steady_clock::time_point last_refill_{};
    bool initialized_ = false;
};

}  // namespace dinero::network
