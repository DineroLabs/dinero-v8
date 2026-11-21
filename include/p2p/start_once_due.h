#pragma once
// "Start this once, the first time it is needed" for work whose need is not
// known at start-up. The caller asks Due(needed) from a periodic path under
// its own lock; it answers true exactly once, the first time `needed` is true,
// however late that is. Started() may be read from any thread without the lock.
//
// It exists because the Orchard snapshot-history workers were decided once, at
// P2P start, when a first-run snapshot had not been imported yet: the need was
// false then, true minutes later, and nobody asked again.
#include <atomic>

namespace dinero::p2p {

class StartOnceDue {
public:
    bool Due(bool needed) {
        if (started_.load() || !needed) return false;
        started_.store(true);
        return true;
    }
    bool Started() const { return started_.load(); }
    void Reset() { started_.store(false); }

private:
    std::atomic<bool> started_{false};
};

}  // namespace dinero::p2p
