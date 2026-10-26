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

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
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
        mgr->set_dialback_private_targets(true);  // everything here is 127.0.0.1
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

TEST(DialBack, StunReflexiveChangeResetsVerification) {
    // The router's public address moved (new lease, laptop on a new network):
    // nothing proven at the old address stands, and the old reflexive
    // candidate is dropped instead of lingering forever.
    Node a("stun-change");
    a.mgr->test_mark_self_verified("203.0.113.5", a.port);
    ASSERT_TRUE(a.mgr->has_proven_reachable());

    auto stun_rows = [&] {
        std::vector<std::string> out;
        for (const auto& st : a.mgr->self_reachability_status()) {
            if (st.c.source == dinero::p2p::CandidateSource::Stun) {
                EXPECT_EQ(st.c.transport, dinero::p2p::Transport::Quic);
                out.push_back(st.c.ip + ":" + std::to_string(st.c.port));
            }
        }
        return out;
    };
    a.mgr->update_stun_candidates({{"162.200.227.214", 45123}});
    EXPECT_EQ(stun_rows(), std::vector<std::string>{"162.200.227.214:45123"});
    EXPECT_TRUE(a.mgr->has_proven_reachable());  // first measurement is not a change

    a.mgr->update_stun_candidates({{"162.200.227.214", 45123}});
    EXPECT_TRUE(a.mgr->has_proven_reachable());  // same answer: still nothing changed

    // A new UDP port alone is the NAT re-mapping an expired binding (normal
    // on non-port-preserving routers), not a new public address.
    a.mgr->update_stun_candidates({{"162.200.227.214", 50001}});
    EXPECT_EQ(stun_rows(), std::vector<std::string>{"162.200.227.214:50001"});
    EXPECT_TRUE(a.mgr->has_proven_reachable()) << "a STUN port change un-verified everything";

    a.mgr->update_stun_candidates({{"99.1.2.3", 45123}});
    EXPECT_EQ(stun_rows(), std::vector<std::string>{"99.1.2.3:45123"});
    EXPECT_FALSE(a.mgr->has_proven_reachable());
}

TEST(DialBack, RefusalUnderLoadDoesNotDeadlock) {
    // A refused reachchk replies to the requester. The reply must not be sent
    // with reach_mutex_ held: handshakes and getaddr answers take reach_mutex_
    // under peers_mutex_, and the reply path takes peers_mutex_.
    Node a("dl-a"), b("dl-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    std::string a_on_b;
    for (int i = 0; i < 100 && a_on_b.empty(); ++i) {
        for (const auto& p : b.mgr->get_connected_peers()) {
            if (p.is_connected && !p.is_outbound) a_on_b = p.to_string();
        }
        if (a_on_b.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_FALSE(a_on_b.empty());
    // A learned (not explicit) address: gossipable_locked must ask reach_mutex_.
    b.mgr->add_advertised_address("44.1.2.3", b.port, /*learned=*/true);

    ASSERT_TRUE(WaitProven(*b.mgr, a_on_b)) << "reachchk is ignored from unproven peers";

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0}, checks{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            while (!stop.load()) { (void)b.mgr->get_advertised_addresses(); ++reads; }
        });
    }
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            P2PMessage m;
            m.command = dinero::MessageCommands::REACH_CHECK;
            m.payload = {1, 0, 0, 0, 0, 0, 0, 0, 0x39, 0x30, 0};  // nonce, port 12345, tcp
            while (!stop.load()) { b.mgr->test_process_message(a_on_b, m); ++checks; }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop.store(true);
    // A stuck loop is the lock inversion; joining it would hang ctest.
    auto joined = std::async(std::launch::async, [&] { for (auto& t : threads) t.join(); });
    if (joined.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::cerr << "DEADLOCK: reachchk refusal vs advertised-address read (reads=" << reads
                  << " checks=" << checks << ")" << std::endl;
        std::_Exit(1);
    }
    std::cout << "reads=" << reads << " checks=" << checks << std::endl;
    EXPECT_GT(checks.load(), 2u);  // refusals actually happened
}

TEST(DialBack, SilentListenerProbeTimesOut) {
    // The requester picks the port. A listener that accepts and never speaks
    // must not pin a verifier's in-flight slot forever: the probe gives up.
    Node a("silent-a"), b("silent-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    const uint16_t silent_port = FreePort();
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(silent_port);
    ASSERT_EQ(::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
    ASSERT_EQ(::listen(s, 8), 0);  // the kernel completes the handshake; nobody reads

    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t nonce = a.mgr->test_send_reachchk(b.Key(), silent_port, {});
    const auto r = WaitResult(*a.mgr, nonce, 30);
    ::close(s);
    ASSERT_TRUE(r.has_value()) << "probe of a silent listener never finished";
    EXPECT_EQ(*r, 2);  // connect failed: no handshake
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(20));
}

TEST(DialBack, NonRoutableRequesterRefusedOutsideRegtest) {
    // Tor inbound peers all arrive from 127.0.0.1: a verifier must never be
    // talked into dialing its own loopback (or any private address) on a port
    // the requester picks.
    Node a("priv-a"), b("priv-b");
    b.mgr->set_dialback_private_targets(false);
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    const uint64_t nonce = a.mgr->test_send_reachchk(b.Key(), a.port, {});
    const auto r = WaitResult(*a.mgr, nonce);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 4);
    EXPECT_TRUE(b.mgr->test_dialback_targets().empty()) << "verifier dialed a loopback requester";
}

TEST(DialBack, PerPeerLimitSurvivesReconnect) {
    // The per-requester budget belongs to the requester's address, not to one
    // connection: reconnecting (new source port) must not reset it.
    Node a("rc-a"), b("rc-b");
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    for (int i = 0; i < 2; ++i) {
        const auto r = WaitResult(*a.mgr, a.mgr->test_send_reachchk(b.Key(), a.port, {}));
        ASSERT_TRUE(r.has_value());
        EXPECT_NE(*r, 1) << "request " << i << " refused before the budget was spent";
    }
    a.mgr->disconnect_peer(b.Key());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", b.port));
    ASSERT_TRUE(WaitProven(*a.mgr, b.Key()));
    const auto r = WaitResult(*a.mgr, a.mgr->test_send_reachchk(b.Key(), a.port, {}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1) << "reconnecting reset the per-requester limit";
}

namespace {
size_t CandidatesAt(const P2PManager& m, const std::string& ip) {
    size_t n = 0;
    for (const auto& st : m.self_reachability_status()) n += st.c.ip == ip;
    return n;
}
}  // namespace

TEST(DialBack, SelfAddressVotesCountPerNetwork) {
    // One /64 (or one /16) is one voice: a single host cannot meet the
    // 3-reporter threshold by itself and seed candidates at will.
    P2PManager m(FreePort());
    for (const char* r : {"2600:1700::1", "2600:1700::2", "2600:1700::3", "2600:1700:5::9"}) {
        m.record_self_address_report("44.1.2.3", r);
    }
    EXPECT_EQ(CandidatesAt(m, "44.1.2.3"), 0u) << "votes from one /32 created a candidate";
    for (const char* r : {"1.1.1.1", "2.2.2.2", "3.3.3.3"}) m.record_self_address_report("44.5.6.7", r);
    EXPECT_EQ(CandidatesAt(m, "44.5.6.7"), 1u);
}

TEST(DialBack, NonAdvertisableVotedAddressIsNotACandidate) {
    P2PManager m(FreePort());
    for (const char* r : {"1.1.1.1", "2.2.2.2", "3.3.3.3"}) m.record_self_address_report("10.1.2.3", r);
    EXPECT_EQ(CandidatesAt(m, "10.1.2.3"), 0u);
    EXPECT_FALSE(m.has_proven_reachable());
}
