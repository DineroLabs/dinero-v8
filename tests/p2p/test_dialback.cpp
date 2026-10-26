// Dial-back verification: a node advertises its own address only after other
// nodes have dialed it back and reached THIS node. These cases drive real
// P2PManagers over loopback TCP.

#include "p2p_manager.h"
#include "daemon/node_identity.h"
#include "network/types.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace {

uint16_t FreePort() {
    int t = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a); ::getsockname(t, reinterpret_cast<sockaddr*>(&a), &len);
    const uint16_t port = ntohs(a.sin_port);
    ::close(t);
    return port;
}

uint64_t Services(bool dialback) {
    uint64_t s = dinero::ServiceFlags::NODE_NETWORK | dinero::ServiceFlags::NODE_DINERO_V2 |
                 dinero::ServiceFlags::NODE_COMPACT_TIMING_V1;
    if (dialback) s |= dinero::ServiceFlags::NODE_DIALBACK;
    return s;
}

struct Node {
    std::filesystem::path dir;
    uint16_t port;
    std::unique_ptr<P2PManager> mgr;
    explicit Node(const char* name, bool dialback = true) : port(FreePort()) {
        dir = std::filesystem::temp_directory_path() /
              ("dialback-" + std::string(name) + "-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        mgr = std::make_unique<P2PManager>(port);
        auto id = std::make_shared<dinero::daemon::NodeIdentity>();
        EXPECT_TRUE(id->initialize(dir.string()));
        mgr->set_node_identity(id);
        mgr->set_service_flags_provider([dialback] { return Services(dialback); });
        mgr->set_height_provider([] { return 10u; });
        mgr->set_quic_enabled(false);
        mgr->set_dialback_min_groups(1);
        EXPECT_TRUE(mgr->start());
    }
    ~Node() {
        mgr->stop();
        mgr.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string Key() const { return "127.0.0.1:" + std::to_string(port); }
};

bool WaitProven(P2PManager& m, const std::string& key, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* p = m.get_peer_info(key); p && p->is_connected && p->identity_proven) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::optional<uint8_t> WaitResult(P2PManager& m, uint64_t nonce, int seconds = 15) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto r = m.test_reach_result(nonce)) return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return std::nullopt;
}

}  // namespace

TEST(DialBack, ReachableNodeBecomesVerified) {
    Node a("ok-a"), b("ok-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    a.mgr->test_add_self_candidate("127.0.0.1", a.port);
    a.mgr->test_run_reachability_tick();
    bool verified = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!verified && std::chrono::steady_clock::now() < deadline) {
        for (const auto& st : a.mgr->self_reachability_status()) {
            if (st.c.ip == "127.0.0.1" && st.c.port == a.port &&
                st.state == dinero::p2p::VerifyState::Verified) verified = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(verified) << "reachable address never became verified";
}

TEST(DialBack, UnreachablePortIsNeverGossiped) {
    Node a("unr-a"), b("unr-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    const uint64_t nonce = a.mgr->test_send_reachchk(b.Key(), FreePort());  // nothing listens
    EXPECT_EQ(WaitResult(*a.mgr, nonce), std::optional<uint8_t>(2));
    // Gossip only carries explicit or verified addresses.
    a.mgr->add_advertised_address("203.0.113.77", a.port, /*learned=*/true);
    for (const auto& [ip, port] : a.mgr->test_local_advertised_addresses()) {
        EXPECT_NE(ip, "203.0.113.77") << "unverified learned address was gossiped";
    }
    a.mgr->test_mark_self_verified("203.0.113.77", a.port);
    bool now_present = false;
    for (const auto& [ip, port] : a.mgr->test_local_advertised_addresses()) {
        if (ip == "203.0.113.77") now_present = true;
    }
    EXPECT_TRUE(now_present) << "verified learned address not gossiped";
}

TEST(DialBack, SharedNatWrongNodeIsIdentityMismatch) {
    Node a("nat-a"), b("nat-b"), c("nat-c");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    // c listens on the requester's IP but is a different node (the Mac/Dell case).
    const uint64_t nonce = a.mgr->test_send_reachchk(b.Key(), c.port);
    EXPECT_EQ(WaitResult(*a.mgr, nonce), std::optional<uint8_t>(3));
}

TEST(DialBack, VerifierDialsOnlyRequesterSourceIp) {
    Node a("src-a"), b("src-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    // Trailing junk bytes cannot smuggle a different target.
    const uint64_t nonce = a.mgr->test_send_reachchk(
        b.Key(), a.port, {0xC0, 0xA8, 0x00, 0x01, 0x08, 0x08, 0x08, 0x08});
    ASSERT_TRUE(WaitResult(*a.mgr, nonce).has_value());
    const auto targets = b.mgr->test_dialback_targets();
    ASSERT_FALSE(targets.empty());
    for (const auto& t : targets) {
        EXPECT_EQ(t, "127.0.0.1:" + std::to_string(a.port));
    }
}

TEST(DialBack, RateLimitRefusesWithoutDialing) {
    Node a("rl-a"), b("rl-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    for (int i = 0; i < 2; ++i) {
        const uint64_t n = a.mgr->test_send_reachchk(b.Key(), a.port);
        ASSERT_TRUE(WaitResult(*a.mgr, n).has_value());
    }
    const size_t dials_before = b.mgr->test_dialback_targets().size();
    const uint64_t third = a.mgr->test_send_reachchk(b.Key(), a.port);
    EXPECT_EQ(WaitResult(*a.mgr, third), std::optional<uint8_t>(1));
    EXPECT_EQ(b.mgr->test_dialback_targets().size(), dials_before) << "rate-limited request still dialed";
}

TEST(DialBack, OldPeerWithoutBitNeverReceivesReachchk) {
    Node a("old-a"), old("old-b", /*dialback=*/false);
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", old.port));
    ASSERT_TRUE(WaitProven(*a.mgr, old.Key()));
    a.mgr->test_add_self_candidate("127.0.0.1", a.port);
    a.mgr->test_run_reachability_tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(old.mgr->test_dialback_targets().empty());
    EXPECT_EQ(a.mgr->test_reach_checks_sent(), 0u) << "sent reachchk to a peer without NODE_DIALBACK";
}
