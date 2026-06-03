#pragma once

// issue #214: in-daemon staleness recovery — the DECISION half, factored out of
// P2PService::MaybeRecoverStaleTip so it can be unit-tested deterministically
// with injected time and no sockets / no DaemonContext singleton.
//
// A node that has finished syncing relies on inv/headers announcements to learn
// about new blocks. If those stop arriving on long-lived connections (stale peer
// state), the best header freezes and the node silently falls behind while it
// still believes it is "Synced". This decides WHEN to re-issue getheaders to
// pull what the stale connections stopped pushing; the caller does the actual
// network I/O. Keeping the two apart is what makes the trigger logic testable
// without reproducing the whole network.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dinero::daemon {

// The action the caller should take this tick. Only SEND_GETHEADERS performs
// network I/O; every other value is a no-op (returned so tests can assert the
// exact branch taken, not merely "did it send").
enum class StaleTipAction : std::uint8_t {
    RESET,            // best header advanced (or first observation) — clock reset
    IDLE,             // no peers, or height 0 (never synced) — nothing to recover
    NOT_STALE_YET,    // header frozen, but not for long enough to act
    RATE_LIMITED,     // would act, but within the getheaders rate-limit window
    SEND_GETHEADERS,  // STALE — re-issue getheaders to all peers
};

// Mutable stall-tracking state. Lives on P2PService; passed by reference so the
// decision both reads and advances it. Default-constructed == "never observed"
// (a default steady_clock::time_point has time_since_epoch()==0, our first-tick
// sentinel).
struct StaleTipState {
    uint32_t last_best_header_height{0};
    std::chrono::steady_clock::time_point last_header_advance_time;
    std::chrono::steady_clock::time_point last_staleness_getheaders;
    int staleness_getheaders_count{0};
};

// Pure decision: given the current best-header height, peer count, and time,
// advance `state` and return what to do. No globals, no I/O — call it with
// synthetic timestamps to test every branch.
//
// Order matters and mirrors the original inline logic exactly:
//   1. header advanced OR first observation -> reset clock                (RESET)
//   2. no peers OR never synced (height 0)                               (IDLE)
//   3. frozen, but < staleness_threshold                          (NOT_STALE_YET)
//   4. would fire, but < staleness_getheaders_interval since last (RATE_LIMITED)
//   5. otherwise: record the probe and tell the caller to send (SEND_GETHEADERS)
inline StaleTipAction decideStaleTipAction(
    uint32_t best_h,
    std::size_t peer_count,
    std::chrono::steady_clock::time_point now,
    std::chrono::seconds staleness_threshold,
    std::chrono::seconds staleness_getheaders_interval,
    StaleTipState& state) {
    // (1) Best header advanced (or first observation): reset the stall clock.
    // A default-constructed time_point has time_since_epoch()==0, which marks the
    // very first tick so we anchor the clock instead of treating it as stalled.
    if (best_h > state.last_best_header_height ||
        state.last_header_advance_time.time_since_epoch().count() == 0) {
        state.last_best_header_height = best_h;
        state.last_header_advance_time = now;
        state.staleness_getheaders_count = 0;
        return StaleTipAction::RESET;
    }

    // (2) Frozen header only counts as a stall with peers AND a real chain — a
    // zero-peer node is handled by reconnect logic, and height 0 means we never
    // synced in the first place.
    if (peer_count == 0 || best_h == 0) {
        return StaleTipAction::IDLE;
    }

    // (3) Not frozen long enough yet.
    if (now - state.last_header_advance_time < staleness_threshold) {
        return StaleTipAction::NOT_STALE_YET;
    }

    // (4) Rate-limit recovery actions.
    if (now - state.last_staleness_getheaders < staleness_getheaders_interval) {
        return StaleTipAction::RATE_LIMITED;
    }

    // (5) Stale: record the probe and tell the caller to re-issue getheaders.
    state.last_staleness_getheaders = now;
    ++state.staleness_getheaders_count;
    return StaleTipAction::SEND_GETHEADERS;
}

}  // namespace dinero::daemon
