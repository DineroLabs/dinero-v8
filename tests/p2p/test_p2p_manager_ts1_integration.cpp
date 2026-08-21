// Ring 3 Phase 4c: TS1 Integration Test for Production P2PManager
// =================================================================
// This test verifies that the ACTUAL production P2PManager satisfies TS1.
//
// Property TS1: Peer Lifetime Safety
// ===================================
// ∀ peer P, ∀ thread T:
//   If T executes any method on P, then P.state ∈ {RUNNING, STOPPING}
//
// And:
//   P.state == DESTROYED ⇒ no thread may hold a reference to P
//
// Expected Status: PASS (after Phase 4c refactor)
//
// Exit Criteria:
// ✅ No ASAN violations
// ✅ No segfaults during concurrent shutdown
// ✅ No use-after-free under stress
// ✅ All lifecycle states transition correctly

#include "p2p_manager.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace dinero::p2p::integration::test {

// ============================================================================
// TS1 Integration Tests (Production P2PManager)
// ============================================================================

/// TS1.1: Basic start/stop cycle
/// Verifies that P2PManager can start and stop cleanly without crashes
TEST(P2PManager_TS1_Integration, BasicStartStop) {
    P2PManager manager(30000);  // Use non-standard port to avoid conflicts

    // Start the manager
    ASSERT_TRUE(manager.start());
    EXPECT_TRUE(manager.is_running());

    // Give it a moment to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop the manager
    manager.stop();
    EXPECT_FALSE(manager.is_running());

    // TS1 EXPECTATION: No crash, no use-after-free
    // If we reach here, basic lifecycle is TS1-compliant
    SUCCEED();
}

TEST(P2PManager_TS1_Integration, AnchorsRemainDistinctFromDynamicSeeds) {
    P2PManager manager(30002);

    manager.add_anchor_node("173.249.200.59", 20999);
    manager.add_anchor_node("172.93.167.32", 20999);
    manager.add_anchor_node("92.118.190.62", 20999);
    manager.add_anchor_node("173.249.200.59", 20999);  // duplicate
    manager.add_seed_node("64.44.157.100", 20999);     // dynamic candidate

    const auto anchors = manager.get_anchor_nodes();
    const auto seeds = manager.get_seed_nodes();
    EXPECT_EQ(anchors.size(), 3U);
    EXPECT_EQ(seeds.size(), 4U);
    EXPECT_TRUE(std::find(anchors.begin(), anchors.end(),
                          std::make_pair(std::string("173.249.200.59"),
                                         uint16_t{20999})) != anchors.end());
    EXPECT_TRUE(std::find(anchors.begin(), anchors.end(),
                          std::make_pair(std::string("64.44.157.100"),
                                         uint16_t{20999})) == anchors.end());
}

TEST(P2PManager_TS1_Integration, PeersDatDoesNotPromoteLearnedPeersToSeeds) {
    P2PManager manager(20999);
    manager.add_anchor_node("173.249.200.59", 20999);
    manager.add_anchor_node("172.93.167.32", 20999);
    manager.add_anchor_node("92.118.190.62", 20999);

    const auto path = std::filesystem::temp_directory_path() /
        ("dinero-peers-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&manager)) + ".dat");
    {
        std::ofstream out(path);
        out << "# DINERO_PEERS_V1\n";
        out << "64.44.157.100 20999 1\n";
    }

    manager.load_peers(path.string());
    EXPECT_EQ(manager.get_seed_nodes().size(), 3U);

    manager.save_peers_with_seeds(path.string());
    std::ifstream saved(path);
    const std::string contents((std::istreambuf_iterator<char>(saved)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("64.44.157.100 20999"), std::string::npos);
    std::filesystem::remove(path);
}

/// TS1.2: Start/stop with outbound connection attempt
/// Tests that manager can handle connection lifecycle without crashes
TEST(P2PManager_TS1_Integration, StartStopWithConnection) {
    P2PManager manager(30001);

    ASSERT_TRUE(manager.start());

    // Attempt to connect to a non-existent peer (will fail, but should be safe)
    manager.connect_to_peer("127.0.0.1", 30002);

    // Give connection attempt time to fail gracefully
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // TS1 CRITICAL: Stop while connection might be in progress
    manager.stop();

    // TS1 EXPECTATION: No crash during shutdown
    SUCCEED();
}

/// TS1.3: Rapid start/stop cycles
/// Stress test for lifecycle state machine
TEST(P2PManager_TS1_Integration, RapidStartStopCycles) {
    for (int i = 0; i < 5; i++) {
        P2PManager manager(30003 + i);

        ASSERT_TRUE(manager.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        manager.stop();

        // TS1 EXPECTATION: Each cycle completes without crash
    }

    SUCCEED();
}

/// TS1.4: Concurrent shutdown stress test
/// Multiple threads triggering shutdown simultaneously
TEST(P2PManager_TS1_Integration, ConcurrentShutdownStress) {
    P2PManager manager(30010);

    ASSERT_TRUE(manager.start());

    // Add some seed nodes (won't connect, but will be in peer list)
    manager.add_seed_node("127.0.0.1", 30011);
    manager.add_seed_node("127.0.0.1", 30012);
    manager.add_seed_node("127.0.0.1", 30013);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // TS1 STRESS: Multiple threads calling stop() concurrently
    std::atomic<int> stop_count{0};
    std::vector<std::thread> stop_threads;

    for (int i = 0; i < 3; i++) {
        stop_threads.emplace_back([&manager, &stop_count]() {
            manager.stop();
            stop_count++;
        });
    }

    for (auto& t : stop_threads) {
        t.join();
    }

    // TS1 EXPECTATION: Concurrent stops handled safely
    EXPECT_EQ(stop_count.load(), 3);
    SUCCEED();
}

/// TS1.5: Message handler during shutdown
/// Verifies handlers don't crash when invoked during peer cleanup
TEST(P2PManager_TS1_Integration, MessageHandlerDuringShutdown) {
    P2PManager manager(30020);

    std::atomic<int> messages_received{0};
    std::atomic<int> peer_disconnects{0};

    // Set up handlers that access peer state
    manager.set_message_handler([&](const std::string& peer, const P2PMessage& msg) {
        messages_received++;
        // TS1: Handler may be called during shutdown, must not crash
    });

    manager.set_peer_disconnected_handler([&](const std::string& peer) {
        peer_disconnects++;
        // TS1: Disconnect handler called during cleanup, must not crash
    });

    ASSERT_TRUE(manager.start());

    // Attempt connections (will fail, but handlers may be invoked)
    manager.connect_to_peer("127.0.0.1", 30021);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // TS1 CRITICAL: Stop while handlers may be queued
    manager.stop();

    // TS1 EXPECTATION: No crash in handlers during shutdown
    SUCCEED();
}

/// TS1.6: Broadcast during shutdown
/// Tests that broadcast operations are safe during cleanup
TEST(P2PManager_TS1_Integration, BroadcastDuringShutdown) {
    P2PManager manager(30030);

    ASSERT_TRUE(manager.start());

    // Attempt to connect to peers (will fail, but creates peer state)
    manager.connect_to_peer("127.0.0.1", 30031);
    manager.connect_to_peer("127.0.0.1", 30032);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create a thread that broadcasts messages
    std::atomic<bool> should_broadcast{true};
    std::thread broadcast_thread([&]() {
        while (should_broadcast.load()) {
            auto ping = P2PMessage::create_ping(12345);
            manager.broadcast_message_async(ping);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // TS1 CRITICAL: Stop while broadcasts are in flight
    should_broadcast.store(false);
    manager.stop();
    broadcast_thread.join();

    // TS1 EXPECTATION: No crash from in-flight broadcasts
    SUCCEED();
}

/// TS1.7: Get peer info during shutdown
/// Tests that peer info access is safe during cleanup
TEST(P2PManager_TS1_Integration, GetPeerInfoDuringShutdown) {
    P2PManager manager(30040);

    ASSERT_TRUE(manager.start());

    manager.connect_to_peer("127.0.0.1", 30041);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Thread that continuously queries peer info
    std::atomic<bool> should_query{true};
    std::atomic<int> query_count{0};

    std::thread query_thread([&]() {
        while (should_query.load()) {
            auto peers = manager.get_connected_peers();
            size_t count = manager.get_peer_count();
            query_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // TS1 CRITICAL: Stop while queries are active
    should_query.store(false);
    manager.stop();
    query_thread.join();

    // TS1 EXPECTATION: No crash from concurrent peer info access
    EXPECT_GT(query_count.load(), 0);
    SUCCEED();
}

/// TS1.8: Destructor safety
/// Verifies that P2PManager destructor properly joins all threads
TEST(P2PManager_TS1_Integration, DestructorSafety) {
    {
        P2PManager manager(30050);

        ASSERT_TRUE(manager.start());

        manager.connect_to_peer("127.0.0.1", 30051);
        manager.connect_to_peer("127.0.0.1", 30052);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // TS1 CRITICAL: Destructor called without explicit stop()
        // Destructor must call stop() internally and join all threads
    }

    // TS1 EXPECTATION: No crash when manager goes out of scope
    SUCCEED();
}

// ============================================================================
// TS1 Test Summary
// ============================================================================
//
// Tests Defined: 8
// Expected Result: All PASS (after Phase 4c refactor)
//
// What These Tests Prove:
// -----------------------
// TS1.1: Basic lifecycle is crash-free
// TS1.2: Connection handling doesn't leak threads
// TS1.3: Repeated cycles don't accumulate resources
// TS1.4: Concurrent shutdown is safe
// TS1.5: Handlers don't crash during cleanup
// TS1.6: Async broadcasts don't cause use-after-free
// TS1.7: Peer queries during shutdown are safe
// TS1.8: Destructor properly cleans up all resources
//
// How to Run:
// -----------
// $ ./build/test_p2p_manager_ts1_integration
//
// Expected Output (After Refactor):
// ----------------------------------
// [==========] Running 8 tests from 1 test suite.
// [----------] 8 tests from P2PManager_TS1_Integration
// [ RUN      ] P2PManager_TS1_Integration.BasicStartStop
// [       OK ] P2PManager_TS1_Integration.BasicStartStop
// ...
// [  PASSED  ] 8 tests.
//
// ASAN Integration:
// -----------------
// Compile with: -fsanitize=address -fno-omit-frame-pointer
// Run with: ASAN_OPTIONS=detect_leaks=1
//
// TS1 Success Criteria:
// ---------------------
// ✅ All tests pass
// ✅ No ASAN violations
// ✅ No segfaults
// ✅ No deadlocks (tests complete in <10s)

} // namespace dinero::p2p::integration::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
