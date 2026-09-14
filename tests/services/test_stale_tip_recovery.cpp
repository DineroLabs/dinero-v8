// Deterministic regression test for issue #214 in-daemon staleness recovery,
// extended for issue #738 (self-mining minority tip).
//
// This drives the PURE decision (decideStaleTipAction) that P2PService's
// MaybeRecoverStaleTip delegates to. It reproduces the *failure condition* the
// fix targets — no headers learned from peers while peers are connected —
// without sockets, daemons, sleeps, or the DaemonContext singleton, so it is
// fast and never flaky. The integration (real two-node) tests are a separate
// layer (P2PStalenessRecovery, MinorityTipHeaderRelay); this proves the trigger
// logic.
//
// Behavior chain proven here (the real bug -> the fix):
//   peer has more headers, local node behind  -> no peer headers, peers connected
//   initial sync stalls / announcements go quiet -> peer-header counter freezes
//   patched logic detects the stall            -> SEND_GETHEADERS at threshold
//   it keeps re-probing on the rate-limit cadence while still silent
//   headers arrive from a peer                 -> state RESETs, no watchdog needed
//   recovery acts at 600s, before the 900s external height-watchdog
//   (#738) our OWN mined blocks advancing the tip do NOT reset the clock
//
// Threshold rationale (see p2p_service.h): TARGET_SPACING_SEC is 120s and block
// arrival is Poisson, so a 120s threshold would fire on ~37% of normal blocks.
// The threshold is 5x spacing (600s) to keep false fires rare; these tests are
// written RELATIVE to kThreshold so they track the tunable rather than a magic
// number.

#include "daemon/services/stale_tip_recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using std::chrono::seconds;
using std::chrono::steady_clock;
using dinero::daemon::decideStaleTipAction;
using dinero::daemon::headersMessageResetsStaleClock;
using dinero::daemon::StaleTipAction;
using dinero::daemon::StaleTipState;

namespace {

// Mirror the production tunables in include/daemon/services/p2p_service.h.
constexpr seconds kThreshold{600};       // staleness_threshold_ (5x block spacing)
constexpr seconds kInterval{60};         // staleness_getheaders_interval_
constexpr seconds kWatchdogStall{900};   // external dinero-height-watchdog STALL_SECONDS
constexpr seconds kBlockSpacing{120};    // consensus TARGET_SPACING_SEC

const int64_t T = kThreshold.count();    // threshold, in seconds, for relative timing
const int64_t I = kInterval.count();     // rate-limit interval, in seconds

// A deliberately NON-zero base. The decision uses time_since_epoch()==0 as the
// "first observation" sentinel, and a real steady_clock::now() is never ~0, so
// tests must not sit at epoch-0 or every tick would look like the first.
const steady_clock::time_point kBase = steady_clock::time_point(std::chrono::hours(24));
steady_clock::time_point at(int64_t s) { return kBase + seconds(s); }

// Decide with the production tunables; per-test state passed in. `events` is
// the caller's monotonically increasing count of `headers` messages processed
// from peers (P2PService::peer_header_events_).
StaleTipAction decide(uint32_t height, uint64_t events, size_t peers,
                      steady_clock::time_point now, StaleTipState& st) {
    return decideStaleTipAction(height, events, peers, now, kThreshold, kInterval, st);
}

}  // namespace

// The very first observation anchors the stall clock and never recovers.
TEST(StaleTipRecovery, FirstObservationAnchorsClock) {
    StaleTipState st;
    EXPECT_EQ(decide(100, 0, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(st.last_best_header_height, 100U);
    EXPECT_EQ(st.last_peer_header_events, 0U);
    EXPECT_EQ(st.last_header_advance_time, at(0));
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// A node that keeps learning headers from peers NEVER fires recovery, even
// across spans far longer than the threshold — this is the healthy baseline
// (watchdog idle too).
TEST(StaleTipRecovery, AdvancingTipNeverRecovers) {
    StaleTipState st;
    EXPECT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(101, 2, 4, at(2 * T), st), StaleTipAction::RESET);   // long span, but peer headers
    EXPECT_EQ(decide(102, 3, 4, at(4 * T), st), StaleTipAction::RESET);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
    EXPECT_EQ(st.last_best_header_height, 102U);
    EXPECT_EQ(st.last_peer_header_events, 3U);
}

// Zero peers is handled by reconnect logic, not staleness recovery — even when
// nothing has been learned for far longer than the threshold.
TEST(StaleTipRecovery, NoPeersIsIdleEvenWhenFrozen) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 1, 0, at(10 * T), st), StaleTipAction::IDLE);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// Height 0 means we never synced; getheaders recovery does not apply.
TEST(StaleTipRecovery, NeverSyncedIsIdle) {
    StaleTipState st;
    ASSERT_EQ(decide(0, 0, 4, at(0), st), StaleTipAction::RESET);  // first obs anchors at h0
    EXPECT_EQ(decide(0, 0, 4, at(10 * T), st), StaleTipAction::IDLE);
}

// Silent peers, but not long enough yet -> wait. A single missed block (one
// block-spacing of silence) must NOT trip recovery.
TEST(StaleTipRecovery, FrozenBelowThresholdWaits) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 1, 4, at(kBlockSpacing.count()), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(100, 1, 4, at(T - 1), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// The core fix: silent peers + threshold reached -> re-issue getheaders.
// The boundary is inclusive (>= threshold fires).
TEST(StaleTipRecovery, FiresAtThreshold) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 1, 4, at(T - 1), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(100, 1, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
    EXPECT_EQ(st.staleness_getheaders_count, 1);
    EXPECT_EQ(st.last_staleness_getheaders, at(T));
}

// While peers stay silent, recovery re-probes on the rate-limit cadence and not
// more often — one getheaders per interval, repeatedly, until headers arrive.
TEST(StaleTipRecovery, RateLimitsRepeatProbes) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 1, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);        // probe 1
    EXPECT_EQ(decide(100, 1, 4, at(T + (I / 2)), st), StaleTipAction::RATE_LIMITED); // too soon
    EXPECT_EQ(decide(100, 1, 4, at(T + I - 1), st), StaleTipAction::RATE_LIMITED);   // still too soon
    EXPECT_EQ(decide(100, 1, 4, at(T + I), st), StaleTipAction::SEND_GETHEADERS);    // probe 2
    EXPECT_EQ(st.staleness_getheaders_count, 2);
}

// End-to-end recovery: stall detected -> getheaders issued -> peer answers with
// headers -> state resets to healthy with the probe counter cleared. This is
// the "recovered in-daemon, external watchdog never had to fire" path.
TEST(StaleTipRecovery, RecoveryResolvesWhenTipAdvances) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 1, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);      // detect + probe
    EXPECT_EQ(decide(100, 1, 4, at(T + I), st), StaleTipAction::SEND_GETHEADERS);  // still silent, probe again
    EXPECT_EQ(st.staleness_getheaders_count, 2);

    // Peer responded to getheaders; headers arrived; best header advanced.
    EXPECT_EQ(decide(150, 2, 4, at(T + (2 * I)), st), StaleTipAction::RESET);
    EXPECT_EQ(st.last_best_header_height, 150U);
    EXPECT_EQ(st.last_peer_header_events, 2U);
    EXPECT_EQ(st.last_header_advance_time, at(T + (2 * I)));
    EXPECT_EQ(st.staleness_getheaders_count, 0);  // back to healthy
}

// Documented residual: the local silence signal cannot distinguish a real
// stall from an unusually long but legitimate quiet gap, so at >= threshold a
// healthy-but-quiet node DOES fire a (harmless) getheaders probe. The threshold
// is set high enough (5x spacing) that this is rare; pin it so the behavior is
// an explicit choice, not a surprise.
TEST(StaleTipRecovery, QuietSyncedNodeProbesAtThreshold) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    // No peer headers for a full threshold window on a healthy node -> probe fires.
    EXPECT_EQ(decide(100, 1, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
    EXPECT_GE(kThreshold.count(), 5 * kBlockSpacing.count());  // rare-by-construction
}

// Layering invariant: the in-daemon recovery is the first responder (acts at
// 600s) and the external height-watchdog (900s) is the backstop. If this ever
// inverts, the watchdog would fire first and the in-daemon path would be dead
// code — so pin the ordering.
TEST(StaleTipRecovery, ActsBeforeExternalWatchdog) {
    static_assert(kThreshold < kWatchdogStall,
                  "in-daemon recovery must trigger before the external watchdog");
    static_assert(kThreshold > kBlockSpacing,
                  "threshold must sit above the normal inter-block gap");
    StaleTipState st;
    ASSERT_EQ(decide(100, 1, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 1, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
    EXPECT_LT(at(T), at(kWatchdogStall.count()));
}

// ─── issue #738: self-mining minority tip ───────────────────────────────────
//
// The mainnet incident: SJ (pool) sat on its own 4-block branch while directly
// connected peers were 13 blocks ahead on a heavier chain. SJ's best header
// advanced every ~5 min from its OWN mined blocks, which under the pre-#738
// rule reset the stall clock each time — so the 600s probe never fired and the
// competing headers were never pulled. Regtest repro: MinorityTipHeaderRelay
// variant live-muted-selfmining.

// Our own mined blocks advance best_h but nothing came from a peer: the clock
// must NOT reset. (Under the old height-keyed rule every one of these ticks
// returned RESET and the count stayed 0 forever.)
TEST(StaleTipRecovery, OwnMinedAdvanceDoesNotResetClock) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 7, 4, at(0), st), StaleTipAction::RESET);
    // Self-mined blocks every T/3 (mainnet: ~5 min cadence vs 600s threshold).
    EXPECT_EQ(decide(101, 7, 4, at(T / 3), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(102, 7, 4, at(2 * T / 3), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(st.last_header_advance_time, at(0));   // clock still anchored at t0
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// ...and the probe fires at the threshold even though the tip kept advancing
// from our own blocks the whole time.
TEST(StaleTipRecovery, ProbeFiresAtThresholdWhileSelfMining) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 7, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(101, 7, 4, at(T / 3), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(102, 7, 4, at(2 * T / 3), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(103, 7, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
    EXPECT_EQ(st.staleness_getheaders_count, 1);
    EXPECT_EQ(st.last_staleness_getheaders, at(T));
}

// Headers learned from a peer DO reset the clock — with or without a best-height
// advance (e.g. inserted side-branch headers). The decision only sees the
// caller's evidence counter; which `headers` messages move it is decided by
// headersMessageResetsStaleClock (#738 follow-up: empty probe replies do not).
TEST(StaleTipRecovery, PeerHeadersResetClock) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 7, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(101, 7, 4, at(T / 2), st), StaleTipAction::NOT_STALE_YET);  // own block

    // Peer headers that advanced our tip.
    EXPECT_EQ(decide(105, 8, 4, at(T / 2 + 1), st), StaleTipAction::RESET);
    EXPECT_EQ(st.last_header_advance_time, at(T / 2 + 1));
    EXPECT_EQ(st.last_peer_header_events, 8U);

    // Peer evidence that did NOT advance our best height still resets.
    EXPECT_EQ(decide(105, 9, 4, at(T / 2 + 2), st), StaleTipAction::RESET);
    EXPECT_EQ(st.last_header_advance_time, at(T / 2 + 2));
    EXPECT_EQ(st.last_best_header_height, 105U);
}

// While a probe is outstanding, continued self-mining must not trigger another
// one inside the rate-limit window (no spam), the next probe waits for the
// interval, and the eventual peer reply resets everything.
TEST(StaleTipRecovery, ProbeNotSpammedWhileSelfMiningAfterFire) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 7, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(103, 7, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);          // probe 1
    EXPECT_EQ(decide(104, 7, 4, at(T + 1), st), StaleTipAction::RATE_LIMITED);         // own block, outstanding
    EXPECT_EQ(decide(105, 7, 4, at(T + I - 1), st), StaleTipAction::RATE_LIMITED);     // still within window
    EXPECT_EQ(st.staleness_getheaders_count, 1);
    EXPECT_EQ(decide(106, 7, 4, at(T + I), st), StaleTipAction::SEND_GETHEADERS);      // probe 2, on cadence
    EXPECT_EQ(st.staleness_getheaders_count, 2);

    // Peer answered the probe with its heavier branch: clock reset, counter cleared.
    EXPECT_EQ(decide(120, 8, 4, at(T + I + 1), st), StaleTipAction::RESET);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
    EXPECT_EQ(st.last_peer_header_events, 8U);
}

// ---------------------------------------------------------------------------
// #738 follow-up (audit 2026-09-14, HIGH-1): the reply to our OWN stale-tip
// probe must not reset the stall clock.
//
// As merged in #742, P2PService counted EVERY processed `headers` message as
// peer evidence — including the empty reply to the probe it had just sent. The
// decision then RESET, zeroed the attempt counter, and the next probe waited a
// full threshold (600 s) instead of the 60 s interval. With the single request
// flight and per-attempt peer rotation, a 10-peer node needed up to ~100 min to
// reach the one peer holding the heavier chain, and the "Stale tip" log line
// always read "attempt 1".
//
// The rule that decides whether a `headers` message feeds the counter is
// headersMessageResetsStaleClock(inserted); the OnHeaders handler bumps
// P2PService::peer_header_events_ only when it returns true. These tests drive
// the decision through THAT rule, so a regression in the rule fails here.
// ---------------------------------------------------------------------------

namespace {

struct ProbeRecord {
    int64_t t;          // seconds since kBase
    std::size_t peer;   // peer index that won the single flight
    int attempt;        // staleness_getheaders_count as logged
};

// Simulates MaybeRecoverStaleTip's per-attempt peer rotation plus the OnHeaders
// bump rule: `peer_count` peers, each probe answered within the same tick by a
// `headers` message that inserted `inserted_per_reply` headers. That reply is
// the only `headers` traffic on the wire in this scenario.
std::vector<ProbeRecord> simulateProbes(std::size_t peer_count,
                                        std::size_t inserted_per_reply,
                                        int64_t until_s,
                                        StaleTipState& st,
                                        uint64_t& events) {
    std::vector<ProbeRecord> probes;
    std::size_t cursor = 0;
    for (int64_t t = 1; t <= until_s; ++t) {
        if (decide(100, events, peer_count, at(t), st) != StaleTipAction::SEND_GETHEADERS) {
            continue;
        }
        const std::size_t peer = cursor++ % peer_count;  // first eligible peer wins the flight
        probes.push_back({t, peer, st.staleness_getheaders_count});
        if (headersMessageResetsStaleClock(inserted_per_reply)) {
            ++events;
        }
    }
    return probes;
}

}  // namespace

// Peers 0-8 answer every probe with empty headers (they sit on our branch or
// behind). The probe must still reach peer 9 within threshold + 9*interval, and
// the attempt number must count 1..10 across those probes.
TEST(StaleTipRecovery, EmptyProbeReplyDoesNotResetClock) {
    EXPECT_FALSE(headersMessageResetsStaleClock(0));
    EXPECT_TRUE(headersMessageResetsStaleClock(1));

    StaleTipState st;
    uint64_t events = 7;
    ASSERT_EQ(decide(100, events, 10, at(0), st), StaleTipAction::RESET);

    const int64_t deadline = T + 9 * I;
    const auto probes = simulateProbes(/*peer_count=*/10, /*inserted_per_reply=*/0, deadline, st, events);

    ASSERT_EQ(probes.size(), 10U) << "expected one probe per interval after the threshold";
    for (std::size_t i = 0; i < probes.size(); ++i) {
        EXPECT_EQ(probes[i].peer, i) << "rotation must advance one peer per attempt";
        EXPECT_EQ(probes[i].attempt, static_cast<int>(i) + 1) << "log attempt number must be real";
        EXPECT_EQ(probes[i].t, T + static_cast<int64_t>(i) * I);
    }
    EXPECT_EQ(probes.back().peer, 9U);
    EXPECT_LE(probes.back().t, deadline);
    EXPECT_EQ(st.staleness_getheaders_count, 10);
    EXPECT_EQ(events, 7U) << "empty probe replies are not peer evidence";
}

// A probe reply that INSERTED headers is exactly what recovery hopes for: it
// resets the clock and clears the attempt counter, so no further probe fires
// inside the next threshold.
TEST(StaleTipRecovery, ProbeReplyWithNewHeadersResetsClock) {
    StaleTipState st;
    uint64_t events = 7;
    ASSERT_EQ(decide(100, events, 10, at(0), st), StaleTipAction::RESET);

    const auto probes = simulateProbes(10, /*inserted_per_reply=*/5, T + 9 * I, st, events);

    ASSERT_EQ(probes.size(), 1U);
    EXPECT_EQ(probes[0].t, T);
    EXPECT_EQ(events, 8U);
    EXPECT_EQ(st.staleness_getheaders_count, 0) << "reset must clear the attempt counter";
    EXPECT_EQ(st.last_peer_header_events, 8U);
    EXPECT_EQ(st.last_header_advance_time, at(T + 1));
}
