// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/clock_source.h"

#include <gtest/gtest.h>

#include <chrono>

using dinero::network::FakeClockSource;
using dinero::network::SystemClockSource;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::minutes;
using std::chrono::seconds;

TEST(ClockSourceTest, system_clock_steady_now_is_monotonic) {
    SystemClockSource c;
    const auto a = c.SteadyNow().time_since_epoch().count();
    const auto b = c.SteadyNow().time_since_epoch().count();
    EXPECT_LE(a, b);
}

TEST(ClockSourceTest, fake_clock_starts_at_epoch) {
    FakeClockSource c;
    EXPECT_EQ(c.SteadyNow().time_since_epoch().count(), 0);
    EXPECT_EQ(c.SystemNow().time_since_epoch().count(), 0);
}

TEST(ClockSourceTest, fake_clock_advance_steady_moves_only_steady) {
    FakeClockSource c;
    const auto sys0_count = c.SystemNow().time_since_epoch().count();
    c.AdvanceSteady(minutes(5));
    const auto expected_steady =
        duration_cast<std::chrono::steady_clock::duration>(minutes(5)).count();
    EXPECT_EQ(c.SteadyNow().time_since_epoch().count(), expected_steady);
    EXPECT_EQ(c.SystemNow().time_since_epoch().count(), sys0_count);
}

TEST(ClockSourceTest, fake_clock_advance_system_moves_only_system) {
    FakeClockSource c;
    const auto steady0_count = c.SteadyNow().time_since_epoch().count();
    c.AdvanceSystem(seconds(90));
    const auto expected_system =
        duration_cast<std::chrono::system_clock::duration>(seconds(90)).count();
    EXPECT_EQ(c.SystemNow().time_since_epoch().count(), expected_system);
    EXPECT_EQ(c.SteadyNow().time_since_epoch().count(), steady0_count);
}
