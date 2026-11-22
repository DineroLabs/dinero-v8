#pragma once
// A bounded retry for reading the wallet's verified history
// (ChainstateService::getRuntimeAccountReplay) from a request handler.
//
// The read adopts a history only when it ends exactly at the node's current
// record of its chain. A block delivered while the read is under way makes it
// refuse ("a block was delivered while the replay was read"). At the tip that
// is one block a minute; while a first run is still catching up it is a block
// every second or two, and a caller that asks once almost always loses. Seen
// on a first run: wallet creation refused for nine minutes after the history
// was verified, until one attempt fell into a gap between blocks.
//
// The rule is not changed here: the same read, with the same checks, is asked
// again. After the first full build the node keeps the built history, so each
// further attempt only reads what arrived since and re-checks; that is why
// retrying is cheap, and why it is done only once such a build exists. Before
// that (the history is still being verified) there is nothing to win, and the
// caller is told to come back.

#include "common/status.h"

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

namespace dinero {

struct RuntimeReplayRetryLimits {
    // How long the caller keeps asking after its first attempt has answered,
    // and how long it waits between attempts. A time limit, not a count: an
    // attempt is normally a short extension, but if the kept history was lost
    // it is a full rebuild (half a minute or more), and a count would let
    // forty of those run inside one request.
    std::chrono::milliseconds budget{10000};
    std::chrono::milliseconds pause{250};
};

// `read()` returns a StatusOr-like value (ok(), status()).
// `built()` says whether the node holds a built history to extend.
// `now()` is a steady clock and `wait(pause)` sleeps; tests pass their own.
template <class Read, class Built, class Now, class Wait>
auto ReadRuntimeReplayWithRetry(Read&& read, Built&& built, Now&& now, Wait&& wait, RuntimeReplayRetryLimits limits = {})
    -> decltype(read()) {
    auto result = read();
    const auto start = now();  // the first attempt (it may be the long build) is not counted
    for (;;) {
        // Only the retryable refusal is asked again; a hard failure is the answer.
        if (result.ok() || result.status() != Status::Invalid) return result;
        if (!built()) return result;  // nothing verified and built yet: retrying cannot win
        if (now() - start >= limits.budget) return result;
        wait(limits.pause);
        result = read();
    }
}

template <class Read, class Built>
auto ReadRuntimeReplayWithRetry(Read&& read, Built&& built, RuntimeReplayRetryLimits limits = {}) -> decltype(read()) {
    return ReadRuntimeReplayWithRetry(std::forward<Read>(read), std::forward<Built>(built),
                                      [] { return std::chrono::steady_clock::now(); },
                                      [](std::chrono::milliseconds pause) { std::this_thread::sleep_for(pause); }, limits);
}

}  // namespace dinero
