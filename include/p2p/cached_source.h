#pragma once
// A value that is expensive to read (it takes a lock others hold for long),
// asked for often and from several threads: read at most once per interval,
// and handed out as a whole value every time.
//
// The Orchard history download's target was kept this way inside a mutable
// lambda, with no lock, and was called from the download's worker and from
// every wallet thread that asks whether the history is ready. Two threads
// then read and wrote the same optional at once: a caller could be handed
// half of the old target and half of the new one, which matches nothing.
//
// The source is never called while this object's lock is held, so a source
// that takes other locks cannot deadlock against a caller that holds them.
// While one thread refreshes, the others get the previous value.

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

namespace dinero::p2p {

template <class T>
class CachedSource {
public:
    using Clock = std::chrono::steady_clock;
    using Read = std::function<std::optional<T>()>;

    CachedSource(Read read, Clock::duration keep) : read_(std::move(read)), keep_(keep) {}

    std::optional<T> Get() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = Clock::now();
            if (now < next_) return latest_;
            next_ = now + keep_;  // this thread refreshes; the others keep the previous value meanwhile
        }
        std::optional<T> fresh = read_();
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = fresh;
        return fresh;
    }

private:
    const Read read_;
    const Clock::duration keep_;
    std::mutex mutex_;
    Clock::time_point next_{};
    std::optional<T> latest_;
};

}  // namespace dinero::p2p
