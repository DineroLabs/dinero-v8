/**
 * Regression tests for StopGate — the exactly-once teardown gate wired into
 * DaemonApp::Stop().
 *
 * Incident (on-device, 2026-07-15): DaemonApp::Stop() guarded only with
 * `if (!started_) return;`, and started_ flips false at the END of teardown.
 * Two threads entered Stop() inside that window (the console showed two
 * interleaved "DaemonApp::Stop entered" sequences with a P2P message handled
 * between them); the second entrant re-drove service teardown while the first
 * was still inside it, destroying services under a live thread —
 * EXC_BAD_ACCESS in a libc++ hash table on an unrelated worker.
 *
 * The gate's contract, exercised here from many threads:
 *  1. exactly ONE caller wins TryEnter() (runs teardown),
 *  2. every other caller BLOCKS until MarkDone() — nobody returns into
 *     "after Stop()" code while teardown is still running,
 *  3. after MarkDone(), TryEnter() returns false immediately.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "daemon/stop_gate.h"

using dinero::StopGate;

TEST(StopGate, ExactlyOneWinnerUnderContention)
{
    for (int round = 0; round < 20; ++round) {
        StopGate gate;
        std::atomic<int> winners{0};
        std::atomic<int> losers{0};

        std::vector<std::thread> threads;
        threads.reserve(8);
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                if (gate.TryEnter()) {
                    winners.fetch_add(1);
                    gate.MarkDone();
                } else {
                    losers.fetch_add(1);
                }
            });
        }
        for (auto& t : threads) t.join();

        EXPECT_EQ(winners.load(), 1) << "round " << round;
        EXPECT_EQ(losers.load(), 7) << "round " << round;
    }
}

TEST(StopGate, LosersBlockUntilTeardownFinishes)
{
    StopGate gate;
    ASSERT_TRUE(gate.TryEnter());  // this thread is the teardown winner

    std::atomic<bool> loser_returned{false};
    std::thread loser([&] {
        EXPECT_FALSE(gate.TryEnter());
        loser_returned.store(true);
    });

    // Teardown still in progress: the loser must NOT have returned. This is
    // the incident's exact hazard — a second Stop() caller proceeding (and
    // destructing members) while the winner is mid-teardown.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(loser_returned.load())
        << "second Stop() caller returned while teardown was still running";

    gate.MarkDone();
    loser.join();
    EXPECT_TRUE(loser_returned.load());
}

TEST(StopGate, AfterDoneEntryReturnsImmediatelyFalse)
{
    StopGate gate;
    ASSERT_TRUE(gate.TryEnter());
    gate.MarkDone();

    const auto begin = std::chrono::steady_clock::now();
    EXPECT_FALSE(gate.TryEnter());
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_TRUE(gate.done());
}

TEST(StopGate, FreshGateReportsNotDone)
{
    StopGate gate;
    EXPECT_FALSE(gate.done());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
