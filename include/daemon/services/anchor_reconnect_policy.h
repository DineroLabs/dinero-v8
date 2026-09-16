#pragma once

// Field incident (2026-09-16): a node started with `-connect=<single-peer>`
// is documented and expected to make outbound connections ONLY to that peer —
// P2PService::Start() correctly skips peers.dat, relay hints, hardcoded seed
// nodes, and DNS seed resolution whenever connect-only mode is requested.
//
// But P2PService's periodic "anchor peer auto-reconnect" probe
// (StartSchedulerTickLoop) is a SEPARATE code path that queries the compiled-in
// anchor list directly and dials any anchor not currently connected, with no
// connect-only check at all. On mainnet/testnet (where the anchor list is
// non-empty) this silently violates the connect-only guarantee a few minutes
// after startup — observed directly: a node started with
// `-connect=127.0.0.1:<port>` against a real mainnet datadir established
// outbound connections to two real anchor peers within about a minute.
//
// This went unnoticed because every existing connect-only test runs on
// regtest, where getAnchorPeers("regtest") is empty by construction — there is
// nothing for the probe to dial, so the missing check is invisible there. It
// only bites when connect-only mode is combined with mainnet/testnet, which no
// existing test exercised.
//
// Pure decision, factored out so it is unit-testable without a daemon,
// sockets, or a live anchor list — same rationale as stale_tip_recovery.h.

#include <string>

namespace dinero::daemon {

// True when the periodic anchor-peer auto-reconnect probe should run this
// tick. `connect_config` is the raw value of the `p2p.connect` config key
// (empty when `-connect=` was not specified). Anchor auto-reconnect must never
// run in connect-only mode: dialing anchors is exactly the automatic peer
// discovery `-connect=` promises to disable.
inline bool shouldRunAnchorAutoReconnect(const std::string& connect_config) {
    return connect_config.empty();
}

}  // namespace dinero::daemon
