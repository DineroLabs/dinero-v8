// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Pure helper functions for RELAY_HINTS Phase 1a eviction policy.
// Extracted so the policy logic is unit-testable without
// instantiating P2PManager.

#pragma once

#include <chrono>

namespace dinero::network {

struct HintEvictionPolicy {
    std::chrono::steady_clock::duration ttl;
    int max_failures;
};

// True if (now - learned_at) > policy.ttl (strict inequality — equal-to is
// the boundary case where the hint just turned old enough to be refreshed
// but not yet evicted).
bool ShouldEvictByTtl(std::chrono::steady_clock::time_point learned_at,
                      std::chrono::steady_clock::time_point now,
                      const HintEvictionPolicy& policy);

// True if consecutive_failures >= policy.max_failures.
bool ShouldEvictByFailure(int consecutive_failures,
                          const HintEvictionPolicy& policy);

}  // namespace dinero::network
