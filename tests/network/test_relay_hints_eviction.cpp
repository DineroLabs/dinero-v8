// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_hints_eviction.h"

#include "network/clock_source.h"

#include <gtest/gtest.h>

#include <chrono>

using dinero::network::FakeClockSource;
using dinero::network::HintEvictionPolicy;
using dinero::network::ShouldEvictByFailure;
using dinero::network::ShouldEvictByTtl;
using std::chrono::minutes;

namespace {
HintEvictionPolicy default_policy() {
    return HintEvictionPolicy{
        .ttl = minutes(15),
        .max_failures = 3,
    };
}
}  // namespace

TEST(RelayHintsEvictionTest, fresh_hint_is_not_evicted) {
    FakeClockSource clock;
    const auto learned = clock.SteadyNow();
    clock.AdvanceSteady(minutes(1));
    EXPECT_FALSE(ShouldEvictByTtl(learned, clock.SteadyNow(), default_policy()));
}

TEST(RelayHintsEvictionTest, ttl_expiry_evicts) {
    FakeClockSource clock;
    const auto learned = clock.SteadyNow();
    clock.AdvanceSteady(minutes(16));
    EXPECT_TRUE(ShouldEvictByTtl(learned, clock.SteadyNow(), default_policy()));
}

TEST(RelayHintsEvictionTest, ttl_boundary_exact_15min_not_yet_evicted) {
    FakeClockSource clock;
    const auto learned = clock.SteadyNow();
    clock.AdvanceSteady(minutes(15));
    EXPECT_FALSE(ShouldEvictByTtl(learned, clock.SteadyNow(), default_policy()));
}

TEST(RelayHintsEvictionTest, failure_under_threshold_keeps_hint) {
    EXPECT_FALSE(ShouldEvictByFailure(2, default_policy()));
}

TEST(RelayHintsEvictionTest, failure_at_threshold_evicts) {
    EXPECT_TRUE(ShouldEvictByFailure(3, default_policy()));
}

TEST(RelayHintsEvictionTest, failure_over_threshold_evicts) {
    EXPECT_TRUE(ShouldEvictByFailure(99, default_policy()));
}
