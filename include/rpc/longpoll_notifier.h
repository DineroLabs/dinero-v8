#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LongPollNotifier — server-side long-polling signal for getblocktemplate
// ─────────────────────────────────────────────────────────────────────────────
//
// Standard Bitcoin miner long-polling pattern: the miner sends
// getblocktemplate with a `longpollid` parameter (the hash of the tip it
// last saw). If that matches the server's current best-block hash, the
// server holds the response open until either (a) the tip changes, or
// (b) a timeout fires. On a tip change, every waiting miner wakes up
// immediately and receives a fresh template — no polling round-trip
// latency.
//
// This class is the pure signaling primitive. It carries no state beyond
// a generation counter (bumped on each committed block) and a
// condition_variable. ChainstateService::notifyBlockConnected bumps the
// generation. The getblocktemplate handler waits on it.
//
// Design notes:
//
//   - Process-global singleton. The notifier has no business logic — it's
//     just a cross-module signaling wire. Threading it through
//     DaemonContext was considered but rejected as overhead for what is
//     effectively a condvar.
//
//   - Opaque generation counter, not best-hash equality. Waiters pass in
//     the generation they last saw; any increment wakes them. This makes
//     the wait loop robust against spurious wakes (condition_variable
//     guarantee) and against two blocks connecting so fast the client
//     can't distinguish them by hash alone (still one wake per block).
//
//   - Timeout selection. The HTTP RPC server's client socket timeout is
//     10 seconds (http_rpc_server.h:56 kClientSocketTimeout). Longpoll
//     waits must return BEFORE that, with enough slack to serialize and
//     ship the response. 8 seconds is the recommended default; callers
//     may pick less.
//
//   - Shutdown. Daemon shutdown flips the shutdown flag and broadcasts.
//     All waiters return false from waitForChange and the handler should
//     fall back to returning the current template (not an error — the
//     tip state is still valid until the daemon actually stops).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace dinero {
namespace rpc {

class LongPollNotifier {
public:
    static LongPollNotifier& instance() {
        static LongPollNotifier inst;
        return inst;
    }

    // Called by ChainstateService::notifyBlockConnected on each committed
    // block. Bumps the generation counter and wakes all waiters.
    void notifyBlockConnected() {
        std::lock_guard<std::mutex> lk(m_);
        generation_.fetch_add(1, std::memory_order_release);
        cv_.notify_all();
    }

    // Opaque token representing current tip generation. Miners pass the
    // value they last saw back into waitForChange.
    uint64_t currentGeneration() const {
        return generation_.load(std::memory_order_acquire);
    }

    // Block until generation advances past `last_seen`, or the timeout
    // expires, or shutdown is called. Returns true iff the wake was
    // caused by a real tip change (not timeout, not shutdown).
    bool waitForChange(uint64_t last_seen, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, timeout, [this, last_seen]() {
            return shutdown_.load(std::memory_order_acquire) ||
                   generation_.load(std::memory_order_acquire) != last_seen;
        });
        return !shutdown_.load(std::memory_order_acquire) &&
               generation_.load(std::memory_order_acquire) != last_seen;
    }

    // Daemon shutdown path: wake every waiter so they can return their
    // RPC responses and release the HTTP socket.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(m_);
            shutdown_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    bool isShuttingDown() const {
        return shutdown_.load(std::memory_order_acquire);
    }

private:
    LongPollNotifier() = default;
    ~LongPollNotifier() = default;
    LongPollNotifier(const LongPollNotifier&) = delete;
    LongPollNotifier& operator=(const LongPollNotifier&) = delete;

    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> shutdown_{false};
    std::mutex m_;
    std::condition_variable cv_;
};

} // namespace rpc
} // namespace dinero
