#pragma once

/**
 * #375 follow-up (CSN throughput): a single background drain worker.
 *
 * The CSN OnUtxoBlock handler used to run proof validation + block accept +
 * checkpoint persistence INLINE on the P2P dispatch thread (~3.3 s/block on a
 * 2-core box), pinning the socket reader: peer Recv-Qs grew unbounded and
 * AssumeUTXO backfill ingested at ~6/min against a ~900/min serving rate
 * (DineroTX e2e run). This primitive moves that work onto one dedicated
 * thread while preserving strict ordering — CSN forest state must apply
 * sequentially, so ONE worker (not a pool) is the correct shape.
 *
 * Contract:
 *  - The caller owns all drained state and its lock. `drain_step` runs on the
 *    worker thread with NO lock held by this class; it locks caller state,
 *    extracts at most one unit of work, unlocks around heavy work, and
 *    returns true if it made progress (worker re-runs it immediately) or
 *    false to sleep until the next Notify().
 *  - Notify() never blocks on drain work and never loses a wakeup: a notify
 *    that arrives while drain_step is running guarantees one more drain pass.
 *  - Stop() (also run by the destructor) wakes the worker and joins; a
 *    drain_step in flight completes first.
 */

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace dinero {
namespace daemon {

class NotifyDrainWorker {
public:
    explicit NotifyDrainWorker(std::function<bool()> drain_step,
                               std::string name = "drain-worker")
        : drain_step_(std::move(drain_step)), name_(std::move(name)) {
        thread_ = std::thread([this]() { Run(); });
    }

    ~NotifyDrainWorker() { Stop(); }

    NotifyDrainWorker(const NotifyDrainWorker&) = delete;
    NotifyDrainWorker& operator=(const NotifyDrainWorker&) = delete;

    // Never blocks on drain work; a notify during a drain pass is latched in
    // `pending_` so the worker runs at least one more pass (no lost wakeups).
    void Notify() {
        {
            std::lock_guard<std::mutex> l(m_);
            pending_ = true;
        }
        cv_.notify_one();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> l(m_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void Run() {
        std::unique_lock<std::mutex> l(m_);
        while (!stop_) {
            cv_.wait(l, [this]() { return pending_ || stop_; });
            if (stop_) break;
            pending_ = false;
            l.unlock();
            // Drain until the step reports no progress. drain_step_ manages
            // its own (caller-owned) locking; this class holds no lock here.
            while (drain_step_()) {
            }
            l.lock();
        }
    }

private:
    std::function<bool()> drain_step_;
    std::string name_;
    std::mutex m_;
    std::condition_variable cv_;
    bool pending_ = false;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace daemon
}  // namespace dinero
