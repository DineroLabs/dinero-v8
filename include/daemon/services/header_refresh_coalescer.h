#pragma once

#include <chrono>

namespace dinero::daemon {

enum class HeaderRefreshAction {
    SEND_NOW,
    QUEUE_TRAILING,
};

struct HeaderRefreshState {
    std::chrono::steady_clock::time_point last_request{};
    bool trailing_request_pending{false};
};

// Coalesce a burst of block announcements into one immediate getheaders and
// one trailing request.  The trailing request is load-bearing: without it, the
// last announcement in a fast mining burst can arrive inside the quiet window
// and remain unknown forever.
inline HeaderRefreshAction noteHeaderAnnouncement(
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds minimum_interval,
        HeaderRefreshState& state) {
    const bool never_requested = state.last_request.time_since_epoch().count() == 0;
    if (never_requested || now - state.last_request >= minimum_interval) {
        state.last_request = now;
        state.trailing_request_pending = false;
        return HeaderRefreshAction::SEND_NOW;
    }

    state.trailing_request_pending = true;
    return HeaderRefreshAction::QUEUE_TRAILING;
}

inline bool takeTrailingHeaderRefresh(
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds minimum_interval,
        HeaderRefreshState& state) {
    if (!state.trailing_request_pending ||
        now - state.last_request < minimum_interval) {
        return false;
    }

    state.last_request = now;
    state.trailing_request_pending = false;
    return true;
}

}  // namespace dinero::daemon
