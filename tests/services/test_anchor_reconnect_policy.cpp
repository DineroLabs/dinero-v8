// Regression for the connect-only isolation gap found 2026-09-16: the
// periodic anchor-peer auto-reconnect probe in P2PService::StartSchedulerTickLoop
// dialed compiled-in anchor peers even when the daemon was started with
// `-connect=<single-peer>`, which is documented to make outbound connections
// exclusively to that peer. Observed live: a node started with
// `-connect=127.0.0.1:<port>` against a real mainnet datadir established
// outbound connections to two real anchor peers within about a minute of
// startup.
//
// This drives the PURE decision (shouldRunAnchorAutoReconnect) the probe now
// delegates to, without a daemon, sockets, or a live anchor list.

#include "daemon/services/anchor_reconnect_policy.h"

#include <gtest/gtest.h>

using dinero::daemon::shouldRunAnchorAutoReconnect;

TEST(AnchorReconnectPolicy, RunsWhenNoConnectFlagGiven) {
    // Normal operation: no `-connect=` was specified, so the empty
    // "p2p.connect" config value means auto-reconnect should proceed.
    EXPECT_TRUE(shouldRunAnchorAutoReconnect(""));
}

TEST(AnchorReconnectPolicy, SkipsWhenConnectOnlySinglePeer) {
    // The exact field scenario: `-connect=127.0.0.1:29999`.
    EXPECT_FALSE(shouldRunAnchorAutoReconnect("127.0.0.1:29999"));
}

TEST(AnchorReconnectPolicy, SkipsWhenConnectOnlyMultiplePeers) {
    // Multiple `-connect=` flags accumulate as a comma-separated value
    // upstream; connect-only mode still applies with more than one target.
    EXPECT_FALSE(shouldRunAnchorAutoReconnect("127.0.0.1:29999,127.0.0.1:30000"));
}
