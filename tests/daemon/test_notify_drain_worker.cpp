/**
 * #375 follow-up: NotifyDrainWorker — the CSN dispatch-thread unpinning
 * primitive. The properties under test ARE the DineroTX findings:
 *
 *  1. NotifierNeverBlocksOnSlowDrain — Notify() must return promptly even
 *     while drain_step is mid-flight (~3.3 s/block validations pinned the
 *     P2P dispatch thread; Recv-Q grew unbounded at ~6/min ingest).
 *  2. DrainRunsOnWorkerThread — heavy work happens OFF the caller's thread.
 *  3. NotifyDuringDrainIsNotLost — a wakeup arriving mid-drain guarantees
 *     one more pass (no missed items).
 *  4. DrainsAllQueuedWork / ordering — repeated true from drain_step keeps
 *     draining without further notifies.
 *  5. StopJoinsPromptly — destructor/Stop() terminates the worker cleanly.
 */

#include "daemon/notify_drain_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

using dinero::daemon::NotifyDrainWorker;
using namespace std::chrono_literals;

namespace {

TEST(NotifyDrainWorker, NotifierNeverBlocksOnSlowDrain) {
    std::atomic<int> processed{0};
    std::atomic<int> queued{0};
    NotifyDrainWorker worker(
        [&]() -> bool {
            if (processed.load() >= queued.load()) return false;
            std::this_thread::sleep_for(200ms);  // a "slow validation"
            processed.fetch_add(1);
            return true;
        },
        "test-slow");

    queued.store(3);
    const auto t0 = std::chrono::steady_clock::now();
    worker.Notify();
    const auto notify_latency = std::chrono::steady_clock::now() - t0;

    // The whole point: the notifier (the P2P dispatch thread) must not pay
    // for the drain. 3 x 200ms of work; Notify must return in << 100ms.
    EXPECT_LT(notify_latency, 100ms)
        << "Notify() blocked on drain work — dispatch-thread pinning is back";

    // The work still completes, just asynchronously.
    for (int i = 0; i < 100 && processed.load() < 3; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(processed.load(), 3) << "queued work must complete on the worker";
}

TEST(NotifyDrainWorker, DrainRunsOnWorkerThread) {
    std::atomic<bool> ran{false};
    std::thread::id drain_tid;
    NotifyDrainWorker worker(
        [&]() -> bool {
            if (ran.exchange(true)) return false;
            drain_tid = std::this_thread::get_id();
            return false;
        },
        "test-tid");

    worker.Notify();
    for (int i = 0; i < 100 && !ran.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(ran.load());
    EXPECT_NE(drain_tid, std::this_thread::get_id())
        << "drain_step ran on the notifier's thread";
}

TEST(NotifyDrainWorker, NotifyDuringDrainIsNotLost) {
    std::atomic<int> passes{0};
    std::atomic<bool> in_first_drain{false};
    std::atomic<bool> release_first{false};
    NotifyDrainWorker worker(
        [&]() -> bool {
            const int pass = passes.fetch_add(1);
            if (pass == 0) {
                in_first_drain.store(true);
                for (int i = 0; i < 500 && !release_first.load(); ++i) {
                    std::this_thread::sleep_for(10ms);
                }
            }
            return false;  // sleep until next notify
        },
        "test-wakeup");

    worker.Notify();
    for (int i = 0; i < 200 && !in_first_drain.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(in_first_drain.load());

    worker.Notify();              // arrives while pass 0 is still running
    release_first.store(true);

    for (int i = 0; i < 200 && passes.load() < 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GE(passes.load(), 2) << "a notify during drain was lost";
}

TEST(NotifyDrainWorker, DrainsAllQueuedWorkWithoutFurtherNotifies) {
    std::mutex m;
    std::deque<int> items{1, 2, 3, 4, 5};
    std::deque<int> drained;
    NotifyDrainWorker worker(
        [&]() -> bool {
            std::lock_guard<std::mutex> l(m);
            if (items.empty()) return false;
            drained.push_back(items.front());
            items.pop_front();
            return true;
        },
        "test-all");

    worker.Notify();  // ONE notify must drain the whole queue in order
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> l(m);
            if (drained.size() == 5) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    std::lock_guard<std::mutex> l(m);
    ASSERT_EQ(drained.size(), 5u);
    EXPECT_EQ(drained, (std::deque<int>{1, 2, 3, 4, 5})) << "drain order violated";
}

TEST(NotifyDrainWorker, StopJoinsPromptly) {
    std::atomic<int> calls{0};
    auto worker = std::make_unique<NotifyDrainWorker>(
        [&]() -> bool {
            calls.fetch_add(1);
            return false;
        },
        "test-stop");
    worker->Notify();
    for (int i = 0; i < 100 && calls.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    const auto t0 = std::chrono::steady_clock::now();
    worker.reset();  // Stop + join via destructor
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2))
        << "destructor did not join promptly";
}

}  // namespace
