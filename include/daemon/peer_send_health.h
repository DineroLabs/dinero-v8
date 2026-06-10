#pragma once

#include <atomic>
#include <cstdint>

namespace dinero {

// issue #241/#214: a peer whose socket stops draining (half-dead connection,
// full TCP send buffer) makes every send to it burn the full SO_SNDTIMEO
// window. Senders then convoy on that socket's per-socket send mutex — the
// waiter queue grows faster than it drains — and block-ingest throughput
// collapses. Two such nodes can gridlock each other mutually (each parked
// sending to a peer that has stopped reading). Evicting the peer closes the
// socket, which makes every queued sender fail fast and recycles the
// connection slot — this is the in-daemon staleness recovery #214 asks for.
//
// Threshold: ~kMaxConsecutiveSendFailures x SEND_TIMEOUT_SEC*2 seconds of
// zero forward progress on the socket before eviction.
inline constexpr uint32_t kMaxConsecutiveSendFailures = 3;

// Record one send outcome for a peer. Returns true when the consecutive
// failure streak has reached the eviction threshold — the caller should
// disconnect the peer. A successful send resets the streak.
//
// Thread-safe: multiple senders may record outcomes concurrently; fetch_add
// gives each failure a unique streak value, and the >= comparison keeps the
// trigger robust if a disconnect races with further queued failures
// (disconnect_peer / cleanup_peer are idempotent).
inline bool RecordSendOutcomeShouldDisconnect(
    std::atomic<uint32_t>& consecutive_send_failures,
    bool send_ok) {
    if (send_ok) {
        consecutive_send_failures.store(0, std::memory_order_relaxed);
        return false;
    }
    const uint32_t streak =
        consecutive_send_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    return streak >= kMaxConsecutiveSendFailures;
}

}  // namespace dinero
