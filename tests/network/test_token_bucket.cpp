// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit test for the TokenBucket rate limiter that backs the relay
// circuit bandwidth caps. Drives a fake (steady_clock) timeline so
// refill behaviour is deterministic.

#include "network/token_bucket.h"

#include <chrono>
#include <cstdio>

using dinero::network::TokenBucket;
using Clock = std::chrono::steady_clock;

static int g_fails = 0;

static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

int main() {
    // Arbitrary non-zero base so time_since_epoch() is non-zero.
    const auto t0 = Clock::time_point{} + std::chrono::hours(1);

    // 1000 B/s refill, 5000 B capacity.
    {
        TokenBucket b(1000.0, 5000.0);
        check(b.TryConsume(5000, t0), "full bucket admits a capacity-sized draw");
        check(!b.TryConsume(1, t0), "drained bucket rejects");
        check(b.TryConsume(1000, t0 + std::chrono::seconds(1)),
              "1s elapsed refills exactly one second of rate");
        check(!b.TryConsume(1, t0 + std::chrono::seconds(1)),
              "rejects once that refill is spent");
        check(b.TryConsume(5000, t0 + std::chrono::seconds(100)),
              "a long idle refills up to capacity");
        check(!b.TryConsume(1, t0 + std::chrono::seconds(100)),
              "refill is capped at capacity — no overflow");
    }

    // Default-constructed (zero-capacity) bucket is disabled — fails open.
    {
        TokenBucket disabled;
        check(disabled.TryConsume(1u << 30, t0),
              "zero-capacity bucket admits everything (unconfigured => no limit)");
    }

    // Per-circuit relay shape: 64 KB/s steady, 256 KB burst.
    {
        TokenBucket pc(64.0 * 1024, 256.0 * 1024);
        check(pc.TryConsume(256 * 1024, t0), "256 KB burst admitted");
        check(!pc.TryConsume(1, t0), "burst exhausted -> reject");
        check(pc.TryConsume(64 * 1024, t0 + std::chrono::seconds(1)),
              "64 KB/s steady refill after the burst");
        check(!pc.TryConsume(64 * 1024 + 1, t0 + std::chrono::seconds(2)),
              "cannot draw more than the steady rate per second");
    }

    if (g_fails) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nALL TOKEN-BUCKET CHECKS PASSED\n");
    return 0;
}
