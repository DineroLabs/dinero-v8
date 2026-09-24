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
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <netinet/tcp.h>
#endif

namespace dinero::p2p::integration::test {

// Keep mock listeners bounded even when the dial path fails before connect.
// A blocking accept() would hide a regression by hanging the test teardown.
int AcceptWithin(int listener, std::chrono::seconds timeout) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval wait{static_cast<time_t>(timeout.count()), 0};
    if (select(listener + 1, &readable, nullptr, nullptr, &wait) != 1) return -1;
    return accept(listener, nullptr, nullptr);
}

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

TEST(P2PManager_TS1_Integration, ShutdownBetweenConnectAndHandlerClosesSocket) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t addr_len = sizeof(bind_addr);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&bind_addr), &addr_len), 0);
    const uint16_t port = ntohs(bind_addr.sin_port);

    P2PManager manager(0);
    std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    bool release = false;
    int dial_fd = -1;
    bool result = true;
    manager.test_set_before_peer_handler_start([&](int fd) {
        std::unique_lock<std::mutex> lock(mutex);
        dial_fd = fd;
        connected = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    std::thread dialer([&] { result = manager.connect_to_peer("127.0.0.1", port); });
    bool reached_gate = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        reached_gate = cv.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return connected; });
    }
    manager.begin_shutdown();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    dialer.join();
    close(listener);
    if (!reached_gate) {
        FAIL() << "outbound connection never reached handler-start gate";
        return;
    }
    const bool fd_open = fcntl(dial_fd, F_GETFD) >= 0;
    const bool guard_left = manager.test_is_connecting_peer("127.0.0.1", port);
    if (fd_open) close(dial_fd);  // Keep the RED test from leaking a descriptor.
    EXPECT_FALSE(result) << "refused handler start must not report a connected peer";
    EXPECT_FALSE(fd_open) << "refused handler start leaked its connected socket";
    EXPECT_FALSE(guard_left) << "refused handler start left a connecting guard";
}

TEST(P2PManager_TS1_Integration, ShutdownBeforeInboundHandlerClosesAcceptedSocket) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t addr_len = sizeof(bind_addr);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&bind_addr), &addr_len), 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
    const int accepted = AcceptWithin(listener, std::chrono::seconds(3));
    ASSERT_GE(accepted, 0);

    P2PManager manager(0);
    std::mutex mutex;
    std::condition_variable cv;
    bool at_gate = false;
    bool release = false;
    manager.test_set_before_peer_handler_start([&](int fd) {
        EXPECT_EQ(fd, accepted);
        std::unique_lock<std::mutex> lock(mutex);
        at_gate = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    std::thread inbound([&] {
        manager.test_handle_incoming_connection(accepted, "127.0.0.1");
    });
    bool reached_gate = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        reached_gate = cv.wait_for(lock, std::chrono::seconds(3),
                                   [&] { return at_gate; });
    }
    manager.begin_shutdown();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    inbound.join();
    const bool fd_open = fcntl(accepted, F_GETFD) >= 0;
    if (fd_open) close(accepted);
    close(client);
    close(listener);
    ASSERT_TRUE(reached_gate) << "inbound connection never reached handler-start gate";
    EXPECT_FALSE(fd_open) << "refused inbound handler leaked its accepted socket";
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
    while (!manager.test_shutdown_requested() &&
           std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stop_requested = manager.test_shutdown_requested();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_first_dial = true;
    }
    gate_cv.notify_all();
    stopper.join();

    EXPECT_TRUE(stop_requested);
    EXPECT_EQ(attempts.load(), 1) << "queued unreachable peer was dialed during shutdown";
}

TEST(P2PManager_TS1_Integration, ShutdownIsSignaledBeforeFinalPeersSave) {
    P2PManager manager(0);
    const auto path = std::filesystem::temp_directory_path() /
        ("dinero-peers-stop-order-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&manager)) + ".dat");
    manager.load_peers(path.string());
    std::mutex mutex;
    std::condition_variable cv;
    bool save_entered = false;
    bool release_save = false;
    manager.test_set_before_final_peers_save([&] {
        std::unique_lock<std::mutex> lock(mutex);
        save_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_save; });
    });
    ASSERT_TRUE(manager.start());
    std::thread stopper([&] { manager.stop(); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = cv.wait_for(lock, std::chrono::seconds(3),
                              [&] { return save_entered; });
    }
    const bool shutdown_visible_during_save = manager.test_shutdown_requested();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_save = true;
    }
    cv.notify_all();
    stopper.join();
    std::filesystem::remove(path);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(shutdown_visible_during_save)
        << "queued dials must be cancelled before persistence can block stop";
}

TEST(P2PManager_TS1_Integration, SlowSeedResolverCannotHoldStop) {
    P2PManager manager(0);
    std::mutex mutex;
    std::condition_variable cv;
    bool resolver_entered = false;
    bool release_resolver = false;
    bool resolver_finished = false;
    std::atomic<int> dials{0};
    manager.test_set_ipv4_resolver([&](const std::string&) -> std::optional<std::string> {
        std::unique_lock<std::mutex> lock(mutex);
        resolver_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_resolver; });
        resolver_finished = true;
        cv.notify_all();
        return "127.0.0.1";
    });
    manager.test_set_connection_manager_dial(
        [&](const std::string&, uint16_t) { ++dials; return false; });
    manager.add_seed_node("slow-seed.example.invalid", 20999);
    ASSERT_TRUE(manager.start());
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = cv.wait_for(lock, std::chrono::seconds(3),
                              [&] { return resolver_entered; });
    }
    // The rescue timer makes the negative control finite if stop regresses to
    // waiting for the underlying system resolver again.
    std::thread rescuer([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2),
                         [&] { return release_resolver; })) {
            release_resolver = true;
            cv.notify_all();
        }
    });
    const auto stop_start = std::chrono::steady_clock::now();
    manager.stop();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_start);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_resolver = true;
    }
    cv.notify_all();
    rescuer.join();
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        finished = cv.wait_for(lock, std::chrono::seconds(3),
                               [&] { return resolver_finished; });
    }
    EXPECT_TRUE(entered);
    EXPECT_TRUE(finished);
    EXPECT_LT(stop_ms.count(), 1500) << "getaddrinfo held the manager thread";
    EXPECT_EQ(dials.load(), 0) << "resolved seed was dialed after shutdown";
}

TEST(P2PManager_TS1_Integration, NumericPeerDialsBeforeSlowDnsSeed) {
    P2PManager manager(0);
    std::mutex mutex;
    std::condition_variable cv;
    bool resolver_entered = false;
    bool release_resolver = false;
    bool resolver_finished = false;
    bool numeric_dialed = false;
    bool dns_dialed = false;
    manager.test_set_ipv4_resolver([&](const std::string&) -> std::optional<std::string> {
        std::unique_lock<std::mutex> lock(mutex);
        resolver_entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_resolver; });
        resolver_finished = true;
        cv.notify_all();
        return "127.0.0.1";
    });
    manager.test_set_connection_manager_dial([&](const std::string& host, uint16_t) {
        if (host == "198.51.100.42") {
            std::lock_guard<std::mutex> lock(mutex);
            numeric_dialed = true;
            cv.notify_all();
        } else if (host == "slow-seed.example.invalid") {
            std::lock_guard<std::mutex> lock(mutex);
            dns_dialed = true;
            cv.notify_all();
        }
        return false;
    });
    manager.add_seed_node("198.51.100.42", 20999);
    manager.add_seed_node("slow-seed.example.invalid", 20999);
    ASSERT_TRUE(manager.start());

    bool dial_before_resolver_release = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(2), [&] { return resolver_entered; });
        dial_before_resolver_release = numeric_dialed;
        release_resolver = true;
    }
    cv.notify_all();
    bool dns_eventually_dialed = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        dns_eventually_dialed = cv.wait_for(lock, std::chrono::seconds(2),
                                            [&] { return dns_dialed; });
    }
    manager.stop();
    bool worker_finished = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        worker_finished = cv.wait_for(lock, std::chrono::seconds(3),
                                      [&] { return resolver_finished; });
    }
    EXPECT_TRUE(worker_finished);
    EXPECT_TRUE(dial_before_resolver_release)
        << "a slow DNS seed stalled a ready numeric outbound peer";
    EXPECT_TRUE(dns_eventually_dialed)
        << "a failed numeric attempt consumed the remaining bootstrap slot";
}

TEST(P2PManager_TS1_Integration, ResolverDeadlineAndProcessCap) {
    P2PManager manager(0);
    const auto drained_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(3);
    while (P2PManager::test_resolver_in_flight() != 0 &&
           std::chrono::steady_clock::now() < drained_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(P2PManager::test_resolver_in_flight(), 0U);
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool release = false;
    manager.test_set_ipv4_resolver([&](const std::string&) -> std::optional<std::string> {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        return "127.0.0.1";
    });
    std::vector<std::thread> waiters;
    for (int i = 0; i < 4; ++i) {
        waiters.emplace_back([&, i] {
            (void)manager.test_resolve_ipv4_for_dial(
                "slow-" + std::to_string(i) + ".invalid", std::chrono::seconds(5));
        });
    }
    bool all_entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        all_entered = cv.wait_for(lock, std::chrono::seconds(3),
                                  [&] { return entered == 4; });
    }
    const auto cap_start = std::chrono::steady_clock::now();
    const auto capped = manager.test_resolve_ipv4_for_dial(
        "fifth.invalid", std::chrono::milliseconds(150));
    const auto cap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cap_start);
    int entered_before_release = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        entered_before_release = entered;
        release = true;
    }
    cv.notify_all();
    for (auto& waiter : waiters) waiter.join();
    const auto release_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(3);
    while (P2PManager::test_resolver_in_flight() != 0 &&
           std::chrono::steady_clock::now() < release_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(all_entered);
    EXPECT_FALSE(capped.has_value());
    EXPECT_EQ(entered_before_release, 4)
        << "cap admitted a fifth stalled resolver worker";
    EXPECT_LT(cap_ms.count(), 100) << "saturated resolver budget did not fail promptly";
    EXPECT_EQ(P2PManager::test_resolver_in_flight(), 0U);
}

TEST(P2PManager_TS1_Integration, ResolverTimeoutDoesNotWaitForSystemLookup) {
    P2PManager manager(0);
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    bool finished = false;
    manager.test_set_ipv4_resolver([&](const std::string&) -> std::optional<std::string> {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        finished = true;
        cv.notify_all();
        return "127.0.0.1";
    });
    std::thread rescuer([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return release; })) {
            release = true;
            cv.notify_all();
        }
    });
    const auto start = std::chrono::steady_clock::now();
    const auto result = manager.test_resolve_ipv4_for_dial(
        "timeout.invalid", std::chrono::milliseconds(150));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    rescuer.join();
    bool worker_finished = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        worker_finished = cv.wait_for(lock, std::chrono::seconds(3),
                                      [&] { return finished; });
    }
    EXPECT_TRUE(entered);
    EXPECT_TRUE(worker_finished);
    EXPECT_FALSE(result.has_value());
    EXPECT_GE(elapsed.count(), 100);
    EXPECT_LT(elapsed.count(), 500);
}

TEST(P2PManager_TS1_Integration, SilentSocksProxyCannotHoldStop) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t bind_len = sizeof(bind_addr);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&bind_addr), &bind_len), 0);

    std::mutex mutex;
    std::condition_variable cv;
    bool greeting_seen = false;
    bool release_proxy = false;
    std::thread proxy([&] {
        int accepted = AcceptWithin(listener, std::chrono::seconds(3));
        if (accepted < 0) return;
        struct timeval timeout{2, 0};
        setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        uint8_t greeting[3]{};
        size_t received = 0;
        while (received < sizeof(greeting)) {
            const ssize_t count = recv(accepted, greeting + received,
                                       sizeof(greeting) - received, 0);
            if (count <= 0) break;
            received += static_cast<size_t>(count);
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            greeting_seen = received == sizeof(greeting) && greeting[0] == 0x05;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_proxy; });
        }
        close(accepted);
    });

    P2PManager manager(0);
    manager.set_onion_proxy("127.0.0.1", ntohs(bind_addr.sin_port), false);
    ASSERT_TRUE(manager.start());
    std::atomic<bool> connected{false};
    std::thread dial([&] {
        connected.store(manager.connect_to_peer("silent-proxy-test.onion", 20999));
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = cv.wait_for(lock, std::chrono::seconds(3),
                              [&] { return greeting_seen; });
    }
    const auto stop_start = std::chrono::steady_clock::now();
    manager.stop();
    dial.join();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_start);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_proxy = true;
    }
    cv.notify_all();
    proxy.join();
    close(listener);

    EXPECT_TRUE(entered) << "test did not reach SOCKS receive after greeting";
    EXPECT_FALSE(connected.load());
    EXPECT_LT(stop_ms.count(), 1500) << "silent SOCKS receive held shutdown";
}

TEST(P2PManager_TS1_Integration, ResponsiveSocksProxyStillConnects) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t bind_len = sizeof(bind_addr);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&bind_addr), &bind_len), 0);

    std::mutex mutex;
    std::condition_variable cv;
    bool release_proxy = false;
    bool proxy_ok = false;
    std::thread proxy([&] {
        int accepted = AcceptWithin(listener, std::chrono::seconds(3));
        if (accepted < 0) return;
        struct timeval timeout{3, 0};
        setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const auto receive_exact = [&](uint8_t* data, size_t size) {
            size_t received = 0;
            while (received < size) {
                const ssize_t got = recv(accepted, data + received, size - received, 0);
                if (got <= 0) return false;
                received += static_cast<size_t>(got);
            }
            return true;
        };
        uint8_t greeting[3]{};
        const uint8_t greeting_reply[2]{0x05, 0x00};
        uint8_t request_header[5]{};
        bool ok = receive_exact(greeting, sizeof(greeting)) &&
                  greeting[0] == 0x05 && greeting[1] == 0x01 && greeting[2] == 0x00 &&
                  send(accepted, greeting_reply, sizeof(greeting_reply), 0) == 2 &&
                  receive_exact(request_header, sizeof(request_header)) &&
                  request_header[0] == 0x05 && request_header[1] == 0x01 &&
                  request_header[3] == 0x03;
        std::vector<uint8_t> endpoint(request_header[4] + 2);
        if (ok) ok = receive_exact(endpoint.data(), endpoint.size());
        const std::string expected_host = "healthy-proxy-test.onion";
        if (ok) {
            ok = std::string(endpoint.begin(), endpoint.end() - 2) == expected_host &&
                 endpoint[endpoint.size() - 2] == static_cast<uint8_t>(20999 >> 8) &&
                 endpoint.back() == static_cast<uint8_t>(20999 & 0xff);
        }
        const uint8_t connect_reply[10]{0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0, 0};
        if (ok) ok = send(accepted, connect_reply, sizeof(connect_reply), 0) == 10;
        {
            std::unique_lock<std::mutex> lock(mutex);
            proxy_ok = ok;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_proxy; });
        }
        close(accepted);
    });

    P2PManager manager(0);
    manager.set_onion_proxy("127.0.0.1", ntohs(bind_addr.sin_port), false);
    ASSERT_TRUE(manager.start());
    bool connected = false;
    bool dial_done = false;
    std::thread dial([&] {
        const bool result = manager.connect_to_peer("healthy-proxy-test.onion", 20999);
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = result;
            dial_done = true;
        }
        cv.notify_all();
    });
    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        completed = cv.wait_for(lock, std::chrono::seconds(3),
                                [&] { return dial_done; });
    }
    manager.stop();
    dial.join();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_proxy = true;
    }
    cv.notify_all();
    proxy.join();
    close(listener);

    EXPECT_TRUE(proxy_ok) << "the proxy did not receive a valid SOCKS5 CONNECT";
    EXPECT_TRUE(completed);
    EXPECT_TRUE(connected) << "a responsive proxy was rejected";
}

TEST(P2PManager_TS1_Integration, FullSocksSendBufferCannotHoldStop) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    int send_buffer = 4096;
    ASSERT_EQ(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF,
                         &send_buffer, sizeof(send_buffer)), 0);
    const int flags = fcntl(sockets[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK), 0);
    std::array<uint8_t, 4096> payload{};
    bool full = false;
    for (int i = 0; i < 4096; ++i) {
        if (send(sockets[0], payload.data(), payload.size(), 0) > 0) continue;
        full = errno == EAGAIN || errno == EWOULDBLOCK;
        break;
    }
    ASSERT_TRUE(full) << "could not fill the local SOCKS send buffer";

    P2PManager manager(0);
    ASSERT_TRUE(manager.start());
    std::mutex mutex;
    std::condition_variable cv;
    bool release_receiver = false;
    std::thread rescuer([&] {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(2), [&] { return release_receiver; });
        close(sockets[1]);
    });
    std::atomic<bool> sent{true};
    std::thread sender([&] {
        sent.store(manager.test_socks5_transfer_all(
            sockets[0], payload.data(), 1, true, std::chrono::seconds(30)));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stop_start = std::chrono::steady_clock::now();
    manager.stop();
    sender.join();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_start);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_receiver = true;
    }
    cv.notify_all();
    rescuer.join();
    close(sockets[0]);

    EXPECT_FALSE(sent.load());
    EXPECT_LT(stop_ms.count(), 1500) << "full SOCKS send buffer held shutdown";
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
