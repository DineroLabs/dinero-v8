// Deterministic regression test for issue #214 in-daemon staleness recovery.
//
// This drives the PURE decision (decideStaleTipAction) that P2PService's
// MaybeRecoverStaleTip delegates to. It reproduces the *failure condition* the
// fix targets — best header frozen while peers are connected — without sockets,
// daemons, sleeps, or the DaemonContext singleton, so it is fast and never
// flaky. The integration (real two-node) test is a separate, follow-up layer;
// this proves the trigger logic.
//
// Behavior chain proven here (the real bug -> the fix):
//   peer has more headers, local node behind  -> best header frozen with peers
//   initial sync stalls / announcements go quiet -> tip stops advancing
//   patched logic detects the stall            -> SEND_GETHEADERS at threshold
//   it keeps re-probing on the rate-limit cadence while still frozen
//   headers arrive, tip advances               -> state RESETs, no watchdog needed
//   recovery acts at 600s, before the 900s external height-watchdog
//
// Threshold rationale (see p2p_service.h): TARGET_SPACING_SEC is 120s and block
// arrival is Poisson, so a 120s threshold would fire on ~37% of normal blocks.
// The threshold is 5x spacing (600s) to keep false fires rare; these tests are
// written RELATIVE to kThreshold so they track the tunable rather than a magic
// number.

#include "daemon/services/stale_tip_recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

using std::chrono::seconds;
using std::chrono::steady_clock;
using dinero::daemon::decideStaleTipAction;
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

// Decide with the production tunables; per-test state passed in.
StaleTipAction decide(uint32_t height, size_t peers, steady_clock::time_point now,
                      StaleTipState& st) {
    return decideStaleTipAction(height, peers, now, kThreshold, kInterval, st);
}

}  // namespace

// The very first observation anchors the stall clock and never recovers.
TEST(StaleTipRecovery, FirstObservationAnchorsClock) {
    StaleTipState st;
    EXPECT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(st.last_best_header_height, 100U);
    EXPECT_EQ(st.last_header_advance_time, at(0));
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// A node that keeps making progress NEVER fires recovery, even across spans far
// longer than the threshold — this is the healthy baseline (watchdog idle too).
TEST(StaleTipRecovery, AdvancingTipNeverRecovers) {
    StaleTipState st;
    EXPECT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(101, 4, at(2 * T), st), StaleTipAction::RESET);   // long span, but advanced
    EXPECT_EQ(decide(102, 4, at(4 * T), st), StaleTipAction::RESET);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
    EXPECT_EQ(st.last_best_header_height, 102U);
}

// Zero peers is handled by reconnect logic, not staleness recovery — even when
// the tip has been frozen far longer than the threshold.
TEST(StaleTipRecovery, NoPeersIsIdleEvenWhenFrozen) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 0, at(10 * T), st), StaleTipAction::IDLE);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// Height 0 means we never synced; getheaders recovery does not apply.
TEST(StaleTipRecovery, NeverSyncedIsIdle) {
    StaleTipState st;
    ASSERT_EQ(decide(0, 4, at(0), st), StaleTipAction::RESET);  // first obs anchors at h0
    EXPECT_EQ(decide(0, 4, at(10 * T), st), StaleTipAction::IDLE);
}

// Frozen, peers present, but not long enough yet -> wait. A single missed block
// (one block-spacing of silence) must NOT trip recovery.
TEST(StaleTipRecovery, FrozenBelowThresholdWaits) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 4, at(kBlockSpacing.count()), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(100, 4, at(T - 1), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(st.staleness_getheaders_count, 0);
}

// The core fix: frozen tip + peers, threshold reached -> re-issue getheaders.
// The boundary is inclusive (>= threshold fires).
TEST(StaleTipRecovery, FiresAtThreshold) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 4, at(T - 1), st), StaleTipAction::NOT_STALE_YET);
    EXPECT_EQ(decide(100, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
    EXPECT_EQ(st.staleness_getheaders_count, 1);
    EXPECT_EQ(st.last_staleness_getheaders, at(T));
}

// While still frozen, recovery re-probes on the rate-limit cadence and not more
// often — one getheaders per interval, repeatedly, until the tip moves.
TEST(StaleTipRecovery, RateLimitsRepeatProbes) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);        // probe 1
    EXPECT_EQ(decide(100, 4, at(T + (I / 2)), st), StaleTipAction::RATE_LIMITED); // too soon
    EXPECT_EQ(decide(100, 4, at(T + I - 1), st), StaleTipAction::RATE_LIMITED);   // still too soon
    EXPECT_EQ(decide(100, 4, at(T + I), st), StaleTipAction::SEND_GETHEADERS);    // probe 2
    EXPECT_EQ(st.staleness_getheaders_count, 2);
}

// End-to-end recovery: stall detected -> getheaders issued -> peer answers, tip
// advances -> state resets to healthy with the probe counter cleared. This is
// the "recovered in-daemon, external watchdog never had to fire" path.
TEST(StaleTipRecovery, RecoveryResolvesWhenTipAdvances) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);      // detect + probe
    EXPECT_EQ(decide(100, 4, at(T + I), st), StaleTipAction::SEND_GETHEADERS);  // still frozen, probe again
    EXPECT_EQ(st.staleness_getheaders_count, 2);

    // Peer responded to getheaders; headers arrived; best header advanced.
    EXPECT_EQ(decide(150, 4, at(T + (2 * I)), st), StaleTipAction::RESET);
    EXPECT_EQ(st.last_best_header_height, 150U);
    EXPECT_EQ(st.last_header_advance_time, at(T + (2 * I)));
    EXPECT_EQ(st.staleness_getheaders_count, 0);  // back to healthy
}

// Documented residual: the local frozen-tip signal cannot distinguish a real
// stall from an unusually long but legitimate quiet gap, so at >= threshold a
// healthy-but-quiet node DOES fire a (harmless) getheaders probe. The threshold
// is set high enough (5x spacing) that this is rare; pin it so the behavior is
// an explicit choice, not a surprise.
TEST(StaleTipRecovery, QuietSyncedNodeProbesAtThreshold) {
    StaleTipState st;
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    // No new block for a full threshold window on a healthy node -> probe fires.
    EXPECT_EQ(decide(100, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
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
    ASSERT_EQ(decide(100, 4, at(0), st), StaleTipAction::RESET);
    EXPECT_EQ(decide(100, 4, at(T), st), StaleTipAction::SEND_GETHEADERS);
    EXPECT_LT(at(T), at(kWatchdogStall.count()));
}
