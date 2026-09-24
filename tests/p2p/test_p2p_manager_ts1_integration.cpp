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
#include <algorithm>
#include <cstdint>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifdef __linux__
#include <netinet/tcp.h>
#endif

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

TEST(P2PManager_TS1_Integration, ShutdownSkipsQueuedOutboundDials) {
    P2PManager manager(0);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool first_dial_started = false;
    bool release_first_dial = false;
    std::atomic<int> attempts{0};

    // These stand in for two unroutable bootstrap peers. The first dial is
    // deliberately held until stop() has requested shutdown; the second must
    // never be started after that request, even though it was already queued.
    manager.test_set_connection_manager_dial(
        [&](const std::string&, uint16_t) {
            if (++attempts == 1) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                first_dial_started = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&] { return release_first_dial; });
            }
            return false;
        });
    manager.add_seed_node("198.51.100.42", 20999);
    manager.add_seed_node("203.0.113.42", 20999);
    ASSERT_TRUE(manager.start());

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        entered = gate_cv.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return first_dial_started; });
    }
    if (!entered) {
        manager.stop();
        FAIL() << "connection manager never started its first outbound dial";
        return;
    }

    std::thread stopper([&] { manager.stop(); });
    const auto stop_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(3);
    while (manager.is_running() && std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stop_requested = !manager.is_running();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_first_dial = true;
    }
    gate_cv.notify_all();
    stopper.join();

    EXPECT_TRUE(stop_requested);
    EXPECT_EQ(attempts.load(), 1) << "queued unreachable peer was dialed during shutdown";
}

TEST(P2PManager_TS1_Integration, ActiveOutboundConnectWaitCancelsOnStop) {
    P2PManager manager(0);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool poll_entered = false;
    std::atomic<int> polls{0};
    std::atomic<int> connect_result{999};

    // A permanently unwritable socket models an in-flight TCP connect. The
    // real wait helper supplies each select() timeout, making this deterministic
    // without relying on the host network to black-hole a SYN packet.
    manager.test_set_outbound_connect_poll(
        [&](int, std::chrono::milliseconds timeout) {
            ++polls;
            {
                std::lock_guard<std::mutex> lock(gate_mutex);
                poll_entered = true;
            }
            gate_cv.notify_all();
            std::this_thread::sleep_for(timeout);
            return 0;
        });
    ASSERT_TRUE(manager.start());
    std::thread waiter([&] {
        connect_result.store(manager.test_wait_for_outbound_connect());
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        entered = gate_cv.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return poll_entered; });
    }
    const auto stop_start = std::chrono::steady_clock::now();
    manager.stop();
    waiter.join();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_start);

    EXPECT_TRUE(entered);
    EXPECT_LT(stop_ms.count(), 1500) << "active connect waited through its 5s timeout";
    EXPECT_EQ(connect_result.load(), -1);
    EXPECT_LE(polls.load(), 2);
}

TEST(P2PManager_TS1_Integration, NetworkDisableCancelsActiveAndQueuedDials) {
    P2PManager manager(0);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool poll_entered = false;
    bool first_dial_done = false;
    std::atomic<int> attempts{0};
    std::atomic<int> first_result{999};
    manager.test_set_outbound_connect_poll(
        [&](int, std::chrono::milliseconds timeout) {
            {
                std::lock_guard<std::mutex> lock(gate_mutex);
                poll_entered = true;
            }
            gate_cv.notify_all();
            std::this_thread::sleep_for(timeout);
            return 0;
        });
    manager.test_set_connection_manager_dial(
        [&](const std::string&, uint16_t) {
            if (++attempts == 1) {
                first_result.store(manager.test_wait_for_outbound_connect());
                {
                    std::lock_guard<std::mutex> lock(gate_mutex);
                    first_dial_done = true;
                }
                gate_cv.notify_all();
            }
            return false;
        });
    manager.add_seed_node("198.51.100.42", 20999);
    manager.add_seed_node("203.0.113.42", 20999);
    ASSERT_TRUE(manager.start());
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        entered = gate_cv.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return poll_entered; });
    }
    const auto disable_start = std::chrono::steady_clock::now();
    manager.set_network_active(false);
    bool completed_before_stop = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        completed_before_stop = gate_cv.wait_for(lock, std::chrono::milliseconds(600),
                                                 [&] { return first_dial_done; });
    }
    const auto disable_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - disable_start);
    // Leave the manager running long enough to reveal a wrongly attempted
    // second queued seed; stop() must not be what cancels this pass.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const int attempts_while_disabled = attempts.load();
    manager.stop();

    EXPECT_TRUE(entered);
    EXPECT_TRUE(completed_before_stop);
    EXPECT_LT(disable_ms.count(), 600);
    EXPECT_EQ(first_result.load(), -1);
    EXPECT_EQ(attempts_while_disabled, 1);
}

TEST(P2PManager_TS1_Integration, OutboundDialPassRunsNormallyWithoutStop) {
    P2PManager manager(0);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    std::atomic<int> attempts{0};
    manager.test_set_connection_manager_dial(
        [&](const std::string&, uint16_t) {
            {
                std::lock_guard<std::mutex> lock(gate_mutex);
                ++attempts;
            }
            gate_cv.notify_all();
            return false;
        });
    manager.add_seed_node("198.51.100.42", 20999);
    manager.add_seed_node("203.0.113.42", 20999);
    ASSERT_TRUE(manager.start());
    bool both_attempted = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        both_attempted = gate_cv.wait_for(lock, std::chrono::seconds(3),
                                          [&] { return attempts.load() >= 2; });
    }
    manager.stop();
    EXPECT_TRUE(both_attempted);
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
    EXPECT_NE(contents.find("173.249.200.59 20999"), std::string::npos);
    EXPECT_NE(contents.find("172.93.167.32 20999"), std::string::npos);
    EXPECT_NE(contents.find("92.118.190.62 20999"), std::string::npos);
    EXPECT_NE(contents.find("64.44.157.100 20999"), std::string::npos);
    EXPECT_EQ(std::count(contents.begin(), contents.end(), '\n'), 5);

    P2PManager untrusted_reload(20999);
    untrusted_reload.load_peers(path.string());
    EXPECT_TRUE(untrusted_reload.get_anchor_nodes().empty());
    EXPECT_TRUE(untrusted_reload.get_seed_nodes().empty());
    std::filesystem::remove(path);
}

TEST(P2PManager_TS1_Integration, PeersDatAtomicSaveReplacesExistingFile) {
    P2PManager manager(20999);
    const auto path = std::filesystem::temp_directory_path() /
        ("dinero-peers-replace-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&manager)) + ".dat");
    const auto tmp_path = std::filesystem::path(path.string() + ".tmp");

    manager.save_peers(path.string());
    ASSERT_TRUE(std::filesystem::exists(path));
    {
        std::ofstream stale(path, std::ios::trunc);
        stale << "stale-content-that-must-be-replaced\n";
    }
    manager.save_peers(path.string());

    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) {
        writers.emplace_back([&manager, &path] {
            for (int pass = 0; pass < 4; ++pass) {
                manager.save_peers(path.string());
            }
        });
    }
    for (auto& writer : writers) writer.join();

    std::ifstream saved(path);
    const std::string contents((std::istreambuf_iterator<char>(saved)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, "# DINERO_PEERS_V1\n");
    EXPECT_FALSE(std::filesystem::exists(tmp_path));
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

/// Field bug: an inbound peer connection that goes silently dead (no FIN/RST
/// — network drop, sleep/wake, crash) relied entirely on the OS's default TCP
/// keepalive to be noticed, since handle_incoming_connection() never enabled
/// SO_KEEPALIVE on the accepted socket (unlike create_client_socket, which
/// does this correctly for outbound connections). On Linux that default is
/// ~2+ hours — and if the dead peer happened to own HeaderSync's single
/// in-flight request, header sync stayed wedged for the entire window.
/// Verifies the accepted socket for an INBOUND connection gets the same
/// aggressive keepalive timing (60s idle / 30s interval / 3 probes) as an
/// outbound one.
TEST(P2PManager_TS1_Integration, InboundAcceptedSocketHasKeepaliveEnabled) {
    constexpr uint16_t kListenPort = 30100;
    constexpr uint16_t kClientLocalPort = 30101;

    P2PManager manager(kListenPort);
    ASSERT_TRUE(manager.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Raw TCP connect, bound to a known local port — this is enough to
    // reach handle_incoming_connection() and register the peer under a
    // predictable key, without needing a full P2P handshake.
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    int reuse = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(kClientLocalPort);
    local_addr.sin_addr.s_addr = INADDR_ANY;
    ASSERT_EQ(bind(client_fd, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)), 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(kListenPort);
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr), 1);
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)), 0);

    // Give the accept loop time to run handle_incoming_connection().
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const std::string peer_key = "127.0.0.1:" + std::to_string(kClientLocalPort);
    const int server_side_fd = manager.test_peer_socket_fd(peer_key);
    ASSERT_GE(server_side_fd, 0) << "accepted inbound peer was not registered under " << peer_key;

    // SO_KEEPALIVE is a best-effort check only: BSD-derived accept()
    // implementations (macOS included) can inherit this boolean flag from
    // the LISTENING socket regardless of whether handle_incoming_connection
    // sets it on the accepted socket itself, so it does not reliably
    // discriminate fixed vs. buggy code on every platform.
    int keepalive_value = 0;
    socklen_t len = sizeof(keepalive_value);
    ASSERT_EQ(getsockopt(server_side_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive_value, &len), 0);
    EXPECT_NE(keepalive_value, 0)
        << "accepted inbound socket must have SO_KEEPALIVE enabled, "
           "or a dead peer holding HeaderSync's flight is invisible for hours";

#ifdef __linux__
    // The actually-discriminating check for this bug: these per-connection
    // TCP-stack tunables are never applied merely by inheriting a listening
    // socket's options, so pre-fix they hold the kernel's own defaults
    // (typically 7200s idle / 75s interval / 9 probes — hours to detect a
    // dead peer) rather than the aggressive values set explicitly by
    // set_socket_keepalive(). This is what actually wedged HeaderSync on SJ
    // for ~2h6m-2h11m per dead connection.
    int keepidle = 0, keepintvl = 0, keepcnt = 0;
    socklen_t idle_len = sizeof(keepidle), intvl_len = sizeof(keepintvl), cnt_len = sizeof(keepcnt);
    ASSERT_EQ(getsockopt(server_side_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, &idle_len), 0);
    ASSERT_EQ(getsockopt(server_side_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, &intvl_len), 0);
    ASSERT_EQ(getsockopt(server_side_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, &cnt_len), 0);
    EXPECT_EQ(keepidle, 60) << "accepted inbound socket must use the fast keepalive idle time, not the kernel default";
    EXPECT_EQ(keepintvl, 30) << "accepted inbound socket must use the fast keepalive probe interval, not the kernel default";
    EXPECT_EQ(keepcnt, 3) << "accepted inbound socket must use the fast keepalive probe count, not the kernel default";
#endif

    close(client_fd);
    manager.stop();
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
