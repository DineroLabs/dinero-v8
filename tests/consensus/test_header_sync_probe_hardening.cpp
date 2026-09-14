// #738 follow-up (audit 2026-09-14): recovery-probe hardening for
// HeaderSyncManager, driven with an injected clock (no sockets, no sleeps).
//
// HIGH-2: HeaderSyncManager grants exactly one header request flight. A
// deliberate stale-tip recovery request has expected_headers==0, so it would
// otherwise get the 15-minute download timeout and a silent peer would block
// recovery from trying another peer. Only stale-tip recovery gets the short
// timeout. Announcement, connection and synchronization refreshes keep the
// normal timeout because a healthy peer may be busy on a slower machine.
//
// gtest EXPECT/ASSERT only (repo ratchet: no new raw assert() under tests/,
// scripts/ci/check_test_assertions.py), unlike the older sibling binaries.

#include "consensus/header_sync.h"
#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"
#include "primitives/uint256.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {

constexpr uint64_t kMs = 1;
constexpr uint64_t kSec = 1000 * kMs;
constexpr uint64_t kMin = 60 * kSec;

// Production tunables this file pins. Mirrors header_sync.h; if either moves
// the test must be revisited deliberately.
constexpr uint64_t kProbeTimeoutMs = 60 * kSec;        // stale-tip recovery
constexpr uint64_t kDownloadTimeoutMs = 15 * kMin;     // HEADERS_DOWNLOAD_TIMEOUT_BASE_MS

BlockHeader MakeHeader(const uint256& prev, uint32_t time) {
    BlockHeader h;
    h.version = 1;
    h.prev_block_hash = prev;
    h.merkle_root = uint256();
    h.timestamp = time;
    h.difficulty = 0x1d00ffff;
    h.nonce = 1;
    h.utreexo_root = uint256();
    return h;
}

struct SwitchCapture {
    std::vector<std::pair<uint64_t, PeerSwitchReason>> calls;
};

// Two peers registered at our own height (a probe ignores the "peer is ahead"
// eligibility check, exactly like the recovery path), deterministic clock.
struct Fixture {
    uint64_t now_ms = 1'000'000 * kSec;  // non-zero so timeout_deadline > 0 is meaningful
    HeaderChainSelector selector;
    HeaderSyncManager mgr{&selector};
    SwitchCapture switches;

    Fixture() {
        SelectParams(Chain::REGTEST);  // header validation consults Params(); idempotent
        mgr.SetTimeSource([this]() { return now_ms; });
        mgr.SetPeerSwitchCallback([this](uint64_t old_peer, PeerSwitchReason reason) {
            switches.calls.emplace_back(old_peer, reason);
        });
        uint256 null_hash;
        null_hash.SetNull();
        selector.AddHeader(MakeHeader(null_hash, 1000000));
        uint256 none;
        none.SetNull();
        mgr.AddPeer(1, 0, none);
        mgr.AddPeer(2, 0, none);
    }

    void Advance(uint64_t ms) { now_ms += ms; }
    bool Begin(uint64_t peer, HeaderRequestMode mode) {
        return mgr.BeginHeadersRequest(peer, mode).has_value();
    }
};

}  // namespace

// The core HIGH-2 regression: peer A wins a probe flight and never answers.
// Sixty-one seconds later a probe to peer B must be accepted.
TEST(HeaderSyncProbeHardening, SilentProbeReleasesFlightAfterProbeTimeout) {
    Fixture f;
    ASSERT_TRUE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY));
    EXPECT_EQ(f.mgr.GetState(), HeaderSyncState::REQUESTING_HEADERS);
    EXPECT_FALSE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "single flight: B refused while A owns it";

    // Just under the probe timeout: still A's flight.
    f.Advance(kProbeTimeoutMs - kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_FALSE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY));
    EXPECT_TRUE(f.switches.calls.empty());

    // 61 s after the probe: flight released, A marked stalled, B accepted.
    f.Advance(2 * kSec);
    f.mgr.Tick(f.now_ms);
    ASSERT_EQ(f.switches.calls.size(), 1U);
    EXPECT_EQ(f.switches.calls[0].first, 1U);
    EXPECT_EQ(f.switches.calls[0].second, PeerSwitchReason::STALL_TIMEOUT);
    EXPECT_TRUE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "probe flight must be free 61 s after a silent probe";
    EXPECT_EQ(f.mgr.GetStats().current_sync_peer, 2U);
    EXPECT_FALSE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "the silent peer is stalled, as before";
}

// A reply inside the window keeps the flight healthy: an empty `headers`
// message from A releases the flight normally and A is NOT penalised.
TEST(HeaderSyncProbeHardening, AnsweredProbeIsNotStalled) {
    Fixture f;
    ASSERT_TRUE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY));
    f.Advance(kProbeTimeoutMs / 2);
    const auto r = f.mgr.ProcessHeadersWithResult(1, {});
    EXPECT_TRUE(r.accepted);
    f.Advance(kProbeTimeoutMs);  // well past the probe deadline of the old flight
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.switches.calls.empty()) << "an answered probe must never be reported as a stall";
    EXPECT_TRUE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "peer A stays eligible";
}

// Equal-height refreshes are allowed to bypass the ahead-only eligibility
// check, but they keep the normal timeout. This is the path used for block
// announcements, peer connection and compact-block synchronization.
TEST(HeaderSyncProbeHardening, OrdinaryRefreshKeepsLongTimeout) {
    Fixture f;
    ASSERT_TRUE(f.Begin(1, HeaderRequestMode::REFRESH));

    f.Advance(kProbeTimeoutMs + kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.switches.calls.empty())
        << "61 s is not a stall for an ordinary refresh";
    EXPECT_EQ(f.mgr.GetStats().current_sync_peer, 1U);
    EXPECT_FALSE(f.Begin(2, HeaderRequestMode::REFRESH));
}

// A real synchronization request from a peer that is ahead also keeps the
// Bitcoin-Core 15-minute budget.
TEST(HeaderSyncProbeHardening, DownloadRequestKeepsLongTimeout) {
    Fixture f;
    uint256 none;
    none.SetNull();
    f.mgr.UpdatePeerBest(1, 1000, none);  // A is 1000 headers ahead
    ASSERT_TRUE(f.Begin(1, HeaderRequestMode::SYNCHRONIZATION));

    f.Advance(kProbeTimeoutMs + kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.switches.calls.empty()) << "61 s is not a stall for a download flight";
    EXPECT_FALSE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY));

    f.Advance(kDownloadTimeoutMs);  // now past 15 min + 1000 ms
    f.mgr.Tick(f.now_ms);
    ASSERT_EQ(f.switches.calls.size(), 1U);
    EXPECT_EQ(f.switches.calls[0].second, PeerSwitchReason::STALL_TIMEOUT);
    EXPECT_TRUE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY));
}

// ---------------------------------------------------------------------------
// #738 follow-up (audit 2026-09-14, MEDIUM-1): a connected peer must not be
// blacklisted from getheaders forever.
//
// After kMaxRecoveryAttempts "header gap" replies daemon_app calls
// MarkPeerMisbehaving(peer); is_misbehaving (like is_stalled) was cleared only
// in AddPeer, i.e. on reconnect. The peer stayed connected, kept relaying
// blocks and txs, yet BeginHeadersRequest returned nullopt for it for the life
// of the connection — including every stale-tip recovery probe. Penalty flags
// now expire after PEER_PENALTY_EXPIRY_MS (10 min); the peer gets another
// getheaders and is re-penalised if it misbehaves again.
// ---------------------------------------------------------------------------

namespace {
constexpr uint64_t kPenaltyExpiryMs = 10 * kMin;  // PEER_PENALTY_EXPIRY_MS
}

TEST(HeaderSyncProbeHardening, MisbehavingFlagExpiresViaTick) {
    Fixture f;
    f.mgr.MarkPeerMisbehaving(1);
    EXPECT_FALSE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "freshly penalised peer is skipped";

    f.Advance(kPenaltyExpiryMs - kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_FALSE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "still inside the penalty window";

    f.Advance(2 * kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "penalty expired: connected peer eligible again";
}

// Expiry must not depend on Tick() having run: BeginHeadersRequest itself
// re-evaluates the penalty (the recovery path calls it directly).
TEST(HeaderSyncProbeHardening, StalledFlagExpiresOnRequestWithoutTick) {
    Fixture f;
    f.mgr.MarkPeerStalled(2);
    EXPECT_FALSE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY));

    f.Advance(kPenaltyExpiryMs + kSec);
    EXPECT_TRUE(f.Begin(2, HeaderRequestMode::STALE_TIP_RECOVERY));
    EXPECT_EQ(f.mgr.GetStats().current_sync_peer, 2U);
}

// Re-offending after expiry re-arms the penalty for a full window.
TEST(HeaderSyncProbeHardening, PenaltyRearmsOnRepeatOffence) {
    Fixture f;
    f.mgr.MarkPeerMisbehaving(1);
    f.Advance(kPenaltyExpiryMs + kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY));
    ASSERT_TRUE(f.mgr.ProcessHeadersWithResult(1, {}).accepted);  // release the flight

    f.mgr.MarkPeerMisbehaving(1);
    f.Advance(kPenaltyExpiryMs / 2);
    f.mgr.Tick(f.now_ms);
    EXPECT_FALSE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY))
        << "second offence must serve a fresh window";
    f.Advance(kPenaltyExpiryMs / 2 + kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.Begin(1, HeaderRequestMode::STALE_TIP_RECOVERY));
}
