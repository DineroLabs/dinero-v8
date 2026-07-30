// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Regression test for the keepalive idle-peer reaping policy
// (P2PManager::classify_idle_peer). Uses FakeClockSource-driven time points
// exclusively — no sleeps, no threads, no sockets — so the 90-second
// boundary is pinned deterministically:
//
//   1. An idle direct INBOUND peer (e.g. a suspended mobile client) is
//      reaped once idle exceeds 90 seconds.
//   2. A peer idle immediately BEFORE the threshold is retained.
//   3. The boundary itself is exclusive: idle == 90s exactly is RETAINED;
//      the first instant strictly past 90s is reaped. (This is the defined
//      boundary — if this test breaks, the on-wire tolerance changed.)
//   4. A last_message_at refresh (any received message, PONGs included)
//      postpones reaping.
//   5. A silent direct OUTBOUND peer remains connected under current
//      policy — the exemption is deliberate, not an oversight.
//
// Also pinned: the pre-existing relay-virtual zombie path classifies
// through the same function, and disconnected peers are never candidates.

#include <gtest/gtest.h>

#include <chrono>

#include "daemon/p2p_manager.h"
#include "network/clock_source.h"

using dinero::network::FakeClockSource;
using IdlePeerAction = P2PManager::IdlePeerAction;

namespace {

constexpr auto kTimeout = P2PManager::kPeerIdleTimeout;  // 90 seconds

// One fake clock per test: last_message_at is captured from the clock, the
// clock is advanced, and "now" is read back from the same clock — the exact
// shape keepalive_loop() sees through ClockSource::SystemNow().
struct FakeTimeline {
    FakeClockSource clock;

    std::chrono::system_clock::time_point mark() { return clock.SystemNow(); }
    void advance(std::chrono::milliseconds delta) { clock.AdvanceSystem(delta); }
    std::chrono::system_clock::time_point now() { return clock.SystemNow(); }
};

IdlePeerAction classify_inbound(std::chrono::system_clock::time_point last,
                                std::chrono::system_clock::time_point now) {
    return P2PManager::classify_idle_peer(/*is_connected=*/true,
                                          /*is_relay_virtual=*/false,
                                          /*is_outbound=*/false, last, now);
}

}  // namespace

// Case 1: an idle inbound (mobile) peer is reaped once past 90 seconds.
TEST(P2PIdlePeerReap, IdleInboundPeerIsReapedPastNinetySeconds) {
    FakeTimeline t;
    const auto last = t.mark();
    t.advance(std::chrono::seconds(91));
    EXPECT_EQ(classify_inbound(last, t.now()),
              IdlePeerAction::kReapDirectInbound);

    // And far past the threshold stays reaped (no wraparound surprises).
    t.advance(std::chrono::hours(1));
    EXPECT_EQ(classify_inbound(last, t.now()),
              IdlePeerAction::kReapDirectInbound);
}

// Case 2: immediately before the threshold the peer is retained.
TEST(P2PIdlePeerReap, InboundPeerRetainedImmediatelyBeforeThreshold) {
    FakeTimeline t;
    const auto last = t.mark();
    t.advance(kTimeout - std::chrono::milliseconds(1));
    EXPECT_EQ(classify_inbound(last, t.now()), IdlePeerAction::kKeep);
}

// Case 3: the defined boundary. idle == 90s exactly is RETAINED (the
// comparison is `idle <= timeout`); the first representable instant beyond
// it is reaped. Both halves asserted so the boundary cannot silently move
// in either direction.
TEST(P2PIdlePeerReap, BoundaryIsExclusiveAtExactlyNinetySeconds) {
    FakeTimeline t;
    const auto last = t.mark();

    t.advance(kTimeout);  // idle == 90s exactly
    EXPECT_EQ(classify_inbound(last, t.now()), IdlePeerAction::kKeep)
        << "a peer at exactly the threshold must be retained";

    t.advance(std::chrono::milliseconds(1));  // idle == 90s + 1ms
    EXPECT_EQ(classify_inbound(last, t.now()),
              IdlePeerAction::kReapDirectInbound)
        << "the first instant strictly past the threshold must reap";
}

// Case 4: a last_message_at refresh (any message — PONGs count, they pass
// through the receive path before dispatch) postpones reaping by a full
// window.
TEST(P2PIdlePeerReap, LastMessageRefreshPostponesReaping) {
    FakeTimeline t;
    auto last = t.mark();

    t.advance(std::chrono::seconds(89));
    EXPECT_EQ(classify_inbound(last, t.now()), IdlePeerAction::kKeep);

    last = t.now();  // the peer spoke (e.g. answered a PING with a PONG)

    t.advance(std::chrono::seconds(89));  // 178s after the ORIGINAL mark
    EXPECT_EQ(classify_inbound(last, t.now()), IdlePeerAction::kKeep)
        << "refresh must restart the idle window";

    t.advance(std::chrono::seconds(2));  // 91s after the refresh
    EXPECT_EQ(classify_inbound(last, t.now()),
              IdlePeerAction::kReapDirectInbound);
}

// Case 5: a silent direct OUTBOUND peer is exempt under current policy.
TEST(P2PIdlePeerReap, SilentOutboundPeerRemainsConnected) {
    FakeTimeline t;
    const auto last = t.mark();
    t.advance(std::chrono::minutes(10));  // far beyond any threshold
    EXPECT_EQ(P2PManager::classify_idle_peer(/*is_connected=*/true,
                                             /*is_relay_virtual=*/false,
                                             /*is_outbound=*/true, last,
                                             t.now()),
              IdlePeerAction::kKeep)
        << "outbound exemption is current policy — if this fails, the "
           "policy changed and the operational consequences need re-review";
}

// Pre-existing behavior pinned: relay-virtual peers past the threshold are
// relay zombies regardless of direction (the relay path has no TCP-level
// death signal, so application-level reaping is their only liveness rule).
TEST(P2PIdlePeerReap, RelayVirtualPeerPastThresholdIsRelayZombie) {
    FakeTimeline t;
    const auto last = t.mark();
    t.advance(std::chrono::seconds(91));
    for (bool outbound : {false, true}) {
        EXPECT_EQ(P2PManager::classify_idle_peer(/*is_connected=*/true,
                                                 /*is_relay_virtual=*/true,
                                                 outbound, last, t.now()),
                  IdlePeerAction::kReapRelayZombie);
    }
}

// A disconnected entry is never a reap candidate, no matter how stale.
TEST(P2PIdlePeerReap, DisconnectedPeerIsNeverACandidate) {
    FakeTimeline t;
    const auto last = t.mark();
    t.advance(std::chrono::hours(24));
    EXPECT_EQ(P2PManager::classify_idle_peer(/*is_connected=*/false,
                                             /*is_relay_virtual=*/false,
                                             /*is_outbound=*/false, last,
                                             t.now()),
              IdlePeerAction::kKeep);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
