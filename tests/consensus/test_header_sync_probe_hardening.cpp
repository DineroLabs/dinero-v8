// #738 follow-up (audit 2026-09-14): recovery-probe hardening for
// HeaderSyncManager, driven with an injected clock (no sockets, no sleeps).
//
// HIGH-2: HeaderSyncManager grants exactly one header request flight. A probe
// (BeginHeadersRequest(peer, probe=true) — the stale-tip recovery and the
// inv-triggered refresh) has expected_headers==0, so its timeout was exactly
// HEADERS_DOWNLOAD_TIMEOUT_BASE_MS = 15 min. A connected-but-silent peer that
// won the flight therefore blocked every other probe and refresh for 15 min.
// A probe expects an immediate `headers` reply (empty or not), so it gets a
// short flight timeout (HEADERS_PROBE_TIMEOUT_MS = 60 s); on expiry the flight
// is released and the peer is marked stalled exactly as for the long timeout.
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
constexpr uint64_t kProbeTimeoutMs = 60 * kSec;        // HEADERS_PROBE_TIMEOUT_MS
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
    bool Begin(uint64_t peer, bool probe) { return mgr.BeginHeadersRequest(peer, probe).has_value(); }
};

}  // namespace

// The core HIGH-2 regression: peer A wins a probe flight and never answers.
// Sixty-one seconds later a probe to peer B must be accepted.
TEST(HeaderSyncProbeHardening, SilentProbeReleasesFlightAfterProbeTimeout) {
    Fixture f;
    ASSERT_TRUE(f.Begin(1, /*probe=*/true));
    EXPECT_EQ(f.mgr.GetState(), HeaderSyncState::REQUESTING_HEADERS);
    EXPECT_FALSE(f.Begin(2, true)) << "single flight: B refused while A owns it";

    // Just under the probe timeout: still A's flight.
    f.Advance(kProbeTimeoutMs - kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_FALSE(f.Begin(2, true));
    EXPECT_TRUE(f.switches.calls.empty());

    // 61 s after the probe: flight released, A marked stalled, B accepted.
    f.Advance(2 * kSec);
    f.mgr.Tick(f.now_ms);
    ASSERT_EQ(f.switches.calls.size(), 1U);
    EXPECT_EQ(f.switches.calls[0].first, 1U);
    EXPECT_EQ(f.switches.calls[0].second, PeerSwitchReason::STALL_TIMEOUT);
    EXPECT_TRUE(f.Begin(2, true)) << "probe flight must be free 61 s after a silent probe";
    EXPECT_EQ(f.mgr.GetStats().current_sync_peer, 2U);
    EXPECT_FALSE(f.Begin(1, true)) << "the silent peer is stalled, as before";
}

// A reply inside the window keeps the flight healthy: an empty `headers`
// message from A releases the flight normally and A is NOT penalised.
TEST(HeaderSyncProbeHardening, AnsweredProbeIsNotStalled) {
    Fixture f;
    ASSERT_TRUE(f.Begin(1, true));
    f.Advance(kProbeTimeoutMs / 2);
    const auto r = f.mgr.ProcessHeadersWithResult(1, {});
    EXPECT_TRUE(r.accepted);
    f.Advance(kProbeTimeoutMs);  // well past the probe deadline of the old flight
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.switches.calls.empty()) << "an answered probe must never be reported as a stall";
    EXPECT_TRUE(f.Begin(1, true)) << "peer A stays eligible";
}

// The short timeout is probe-specific: a real download request from a peer
// that is ahead keeps the Bitcoin-Core 15-minute budget.
TEST(HeaderSyncProbeHardening, DownloadRequestKeepsLongTimeout) {
    Fixture f;
    uint256 none;
    none.SetNull();
    f.mgr.UpdatePeerBest(1, 1000, none);  // A is 1000 headers ahead
    ASSERT_TRUE(f.Begin(1, /*probe=*/false));

    f.Advance(kProbeTimeoutMs + kSec);
    f.mgr.Tick(f.now_ms);
    EXPECT_TRUE(f.switches.calls.empty()) << "61 s is not a stall for a download flight";
    EXPECT_FALSE(f.Begin(2, true));

    f.Advance(kDownloadTimeoutMs);  // now past 15 min + 1000 ms
    f.mgr.Tick(f.now_ms);
    ASSERT_EQ(f.switches.calls.size(), 1U);
    EXPECT_EQ(f.switches.calls[0].second, PeerSwitchReason::STALL_TIMEOUT);
    EXPECT_TRUE(f.Begin(2, true));
}
