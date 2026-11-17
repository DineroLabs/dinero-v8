#pragma once
#include <functional>
#include <stdexcept>
#include <utility>

namespace dinero {

// Cooperative, thread-local cancellation for long runtime wallet replays.
//
// The runtime delivery worker installs a scope around wallet recovery whose
// predicate reports its own stop request. Replay loops that scale with chain
// length call ThrowIfCancelled() so the worker can be joined promptly during
// shutdown instead of running a full genesis replay into the daemon's 30-second
// stop watchdog (whose _exit(1) also skips every later service's clean stop).
//
// Only a thread that installed a scope can be cancelled. Every other caller,
// such as RPC handlers, the offline --upgrade-wallet-index-v3 path and tests
// without a scope, behaves exactly as before. Cancellation is reported as a
// failure before any projection is returned; recovery commits per-store
// prefixes atomically, so a cancelled attempt leaves them retryable.
struct RuntimeReplayCancelled final : std::runtime_error {
    RuntimeReplayCancelled() : std::runtime_error("Runtime wallet replay cancelled for shutdown") {}
};

class RuntimeReplayCancellationScope final {
public:
    explicit RuntimeReplayCancellationScope(std::function<bool()> cancelled)
        : cancelled_(std::move(cancelled)), previous_(current_) { current_ = this; }
    ~RuntimeReplayCancellationScope() { current_ = previous_; }
    RuntimeReplayCancellationScope(const RuntimeReplayCancellationScope&) = delete;
    RuntimeReplayCancellationScope& operator=(const RuntimeReplayCancellationScope&) = delete;

    static void ThrowIfCancelled() {
        for (auto* scope = current_; scope; scope = scope->previous_)
            if (scope->cancelled_ && scope->cancelled_()) throw RuntimeReplayCancelled();
    }

private:
    std::function<bool()> cancelled_;
    RuntimeReplayCancellationScope* const previous_;
    static inline thread_local RuntimeReplayCancellationScope* current_ = nullptr;
};

}  // namespace dinero
