// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_hints_eviction.h"

namespace dinero::network {

bool ShouldEvictByTtl(std::chrono::steady_clock::time_point learned_at,
                      std::chrono::steady_clock::time_point now,
                      const HintEvictionPolicy& policy) {
    return (now - learned_at) > policy.ttl;
}

bool ShouldEvictByFailure(int consecutive_failures,
                          const HintEvictionPolicy& policy) {
    return consecutive_failures >= policy.max_failures;
}

}  // namespace dinero::network
