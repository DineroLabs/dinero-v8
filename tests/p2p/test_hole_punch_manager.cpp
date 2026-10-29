// Relay-coordinated hole punching through P2PManager (Plan 4 Task 5): two
// real managers linked by a relayed QUIC session exchange punchoff / punchans /
// punchgo end to end, then the initiator dials the acceptor's real loopback
// QuicEndpoint while the acceptor probes it. Candidates are [::1]:port through
// test_set_local_punch_candidates: LocalPunchCandidates never yields loopback,
// and a shared 127.0.0.1 is skipped by the same-IPv4 rule.
//
// The punch clock is real steady time plus a test-controlled offset, so
// handshakes run in real time while deadlines and the 30 min backoff can be
// jumped over.

#include "p2p_manager.h"
#include "daemon/node_identity.h"
#include "network/quic_endpoint.h"
#include "network/quic_session.h"
#include "network/relay_tls_keypair.h"
#include "network/types.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using dinero::network::QuicSession;
using dinero::network::QuicSessionOptions;
using dinero::network::UdpAddr;
using dinero::p2p::PunchCandidates;
using namespace std::chrono_literals;

QuicSessionOptions Opts() {
    static std::string cert, key;
    static std::once_flag once;
    std::call_once(once, [] { ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert, &key, nullptr)); });
    QuicSessionOptions o;
    o.certificate_pem = cert;
    o.private_key_pem = key;
    return o;
}

UdpAddr Addr4(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return UdpAddr::FromIPv4(ip, port);
}

UdpAddr Loop6(uint16_t port) {
    uint8_t ip[16] = {};
    ip[15] = 1;
    return UdpAddr::FromIPv6(ip, port);
}

PunchCandidates Cands(uint16_t port) {
    PunchCandidates c;
    c.v6.push_back(Loop6(port));
    return c;
}

std::string Key6(uint16_t port) { return "[::1]:" + std::to_string(port); }

// A relayed link: two QUIC sessions whose writers feed each other, standing in
// for the two legs of a relay circuit.
struct Leg {
    struct Route { std::mutex mu; QuicSession* a{nullptr}; QuicSession* b{nullptr}; };
    std::shared_ptr<Route> route = std::make_shared<Route>();
    std::shared_ptr<QuicSession> client, server;
    Leg(uint16_t client_port, uint16_t server_port) {
        client = std::make_shared<QuicSession>([r = route](std::vector<uint8_t> x) {
            std::lock_guard<std::mutex> l(r->mu); if (r->b) r->b->EnqueueIncomingPacket(std::move(x)); });
        server = std::make_shared<QuicSession>([r = route](std::vector<uint8_t> x) {
            std::lock_guard<std::mutex> l(r->mu); if (r->a) r->a->EnqueueIncomingPacket(std::move(x)); });
        {
            std::lock_guard<std::mutex> l(route->mu);
            route->a = client.get();
            route->b = server.get();
        }
        EXPECT_TRUE(server->StartServer(Addr4(server_port), Addr4(client_port), Opts()));
        EXPECT_TRUE(client->StartClient(Addr4(client_port), Addr4(server_port), Opts()));
    }
    ~Leg() { std::lock_guard<std::mutex> l(route->mu); route->a = route->b = nullptr; }
};

std::shared_ptr<dinero::daemon::NodeIdentity> MakeIdentity() {
    static std::atomic<int> seq{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero-punch-" + std::to_string(::getpid()) + "-" + std::to_string(seq++));
    std::filesystem::create_directories(dir);
    auto id = std::make_shared<dinero::daemon::NodeIdentity>();
    const bool ok = id->initialize(dir.string());
    std::filesystem::remove_all(dir);
    if (!ok) throw std::runtime_error("identity");
    return id;
}

struct Node {
    std::shared_ptr<dinero::daemon::NodeIdentity> id = MakeIdentity();
    std::unique_ptr<P2PManager> mgr;
    std::atomic<int64_t> skew_ms{0};
    explicit Node(bool holepunch = true) {
        mgr = std::make_unique<P2PManager>(0);
        mgr->set_node_identity(id);
        mgr->set_service_flags_provider([holepunch] {
            uint64_t f = dinero::ServiceFlags::NODE_NETWORK | dinero::ServiceFlags::NODE_DINERO_V2 |
                         dinero::ServiceFlags::NODE_COMPACT_TIMING_V1;
            if (holepunch) f |= dinero::ServiceFlags::NODE_HOLEPUNCH | dinero::ServiceFlags::NODE_P2P_QUIC;
            return f;
        });
        mgr->set_height_provider([] { return 10u; });
        mgr->test_set_punch_clock(
            [this] { return std::chrono::steady_clock::now() + std::chrono::milliseconds(skew_ms.load()); });
    }
    ~Node() {
        mgr->test_set_running(true);
        mgr->stop();
    }
    // Starts the real endpoint and advertises [::1]:its port.
    uint16_t Listen() {
        EXPECT_TRUE(mgr->start_quic_endpoint(0));
        const uint16_t port = mgr->quic_endpoint()->Port();
        mgr->test_set_local_punch_candidates(Cands(port));
        return port;
    }
    void Advance(std::chrono::milliseconds d) { skew_ms += d.count(); }
    std::array<uint8_t, 20> NodeId() const { return id->get_node_id_bytes(); }
};

bool WaitProven(P2PManager& m, const std::string& key, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* p = m.get_peer_info(key); p && p->is_connected && p->identity_proven) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

bool Proven(P2PManager& m, const std::string& key) {
    auto* p = m.get_peer_info(key);
    return p && p->is_connected && p->identity_proven;
}

bool Live(P2PManager& m, const std::string& key) {
    auto* p = m.get_peer_info(key);
    return p && p->is_connected && p->lifetime_state.load() == PeerLifetimeState::RUNNING;
}

// Ticks every node until `done` holds or `seconds` of real time pass.
bool TickUntil(std::initializer_list<Node*> nodes, const std::function<bool()>& done, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Node* n : nodes) n->mgr->test_punch_tick();
        if (done()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

void TickAll(std::initializer_list<Node*> nodes) {
    for (Node* n : nodes) n->mgr->test_punch_tick();
}

// A relayed link A -> B (A dialed). Returns {key on A, key on B}.
std::pair<std::string, std::string> Relay(Node& a, Node& b, Leg& leg, uint64_t cid) {
    const auto key_a = a.mgr->test_install_quic_relay_peer(leg.client, "8.8.10.1:20999", cid, b.NodeId());
    const auto key_b = b.mgr->test_install_quic_relay_peer(leg.server, "8.8.10.1:20999", cid + 1, std::nullopt);
    EXPECT_TRUE(WaitProven(*a.mgr, key_a)) << "relayed link never proven on the dialing side";
    EXPECT_TRUE(WaitProven(*b.mgr, key_b)) << "relayed link never proven on the accepting side";
    return {key_a, key_b};
}

std::array<uint8_t, 20> FakeId(uint8_t seed) {
    std::array<uint8_t, 20> id{};
    id.fill(seed);
    return id;
}

// An inbound relayed, identity-proven peer with NODE_HOLEPUNCH that the test
// speaks for through test_process_message (no handler thread).
std::string FakeRelayedPeer(P2PManager& m, const std::array<uint8_t, 20>& id, const std::string& key) {
    m.test_install_virtual_relay_peer(key, "8.8.10.1:20999", 7,
                                      P2PMessage::RelayDirection::TargetToClient, /*is_outbound=*/false);
    m.test_set_peer_proven_identity(key, id, {}, 1);
    m.test_set_peer_services(key, dinero::ServiceFlags::NODE_NETWORK |
                                      dinero::ServiceFlags::NODE_DINERO_V2 |
                                      dinero::ServiceFlags::NODE_HOLEPUNCH);
    return key;
}

}  // namespace

TEST(HolePunchManager, InitiatorOffersAcceptorAnswersAndBothConnectDirect) {
    Node a, b;
    const uint16_t ap = a.Listen(), bp = b.Listen();
    (void)ap;
    Leg leg(25001, 25002);
    Relay(a, b, leg, 0x101);

    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return Proven(*a.mgr, Key6(bp)); }))
        << "A never got a direct QUIC peer at B's punch candidate";
    auto* p = a.mgr->get_peer_info(Key6(bp));
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->quic_direct);
    EXPECT_TRUE(p->is_outbound);
    EXPECT_TRUE(p->punched);
    ASSERT_TRUE(p->punch_expected_id.has_value());
    EXPECT_EQ(*p->punch_expected_id, b.NodeId());
    const auto sa = a.mgr->test_punch_stats(), sb = b.mgr->test_punch_stats();
    EXPECT_EQ(sa.offers_sent, 1u);
    EXPECT_EQ(sb.answers_sent, 1u);
    EXPECT_EQ(sa.gos_sent, 1u);
    EXPECT_EQ(sa.dials, 1u);
    EXPECT_GT(sb.probes_sent, 0u) << "the acceptor never probed the initiator";
    EXPECT_EQ(sa.offers_sent + sb.offers_sent, 1u);
}

// Review Focus 2: both nodes dialed each other through a relay.
TEST(HolePunchManager, OnlyLowerIdInitiatesWhenBothDialed) {
    Node a, b;
    a.mgr->test_set_local_punch_candidates(Cands(25901));
    b.mgr->test_set_local_punch_candidates(Cands(25902));
    Leg ab(25011, 25012), ba(25013, 25014);
    Relay(a, b, ab, 0x201);
    Relay(b, a, ba, 0x301);

    TickUntil({&a, &b}, [&] {
        return a.mgr->test_punch_stats().answers_sent + b.mgr->test_punch_stats().answers_sent > 0;
    }, 5);
    for (int i = 0; i < 20; ++i) { TickAll({&a, &b}); std::this_thread::sleep_for(20ms); }
    const auto sa = a.mgr->test_punch_stats(), sb = b.mgr->test_punch_stats();
    EXPECT_EQ(sa.offers_sent + sb.offers_sent, 1u) << "A sent " << sa.offers_sent << ", B sent " << sb.offers_sent;
    EXPECT_EQ(sa.answers_sent + sb.answers_sent, 1u);
    Node& lower = a.NodeId() < b.NodeId() ? a : b;
    EXPECT_EQ(lower.mgr->test_punch_stats().offers_sent, 1u) << "the higher node id initiated";
}

TEST(HolePunchManager, NoBitNoMessages) {
    Node a;
    Node b(/*holepunch=*/false);
    a.mgr->test_set_local_punch_candidates(Cands(25903));
    b.mgr->test_set_local_punch_candidates(Cands(25904));
    Leg leg(25021, 25022);
    Relay(a, b, leg, 0x401);
    for (int i = 0; i < 50; ++i) { TickAll({&a, &b}); std::this_thread::sleep_for(20ms); }
    EXPECT_EQ(a.mgr->test_punch_stats().offers_sent, 0u);
    EXPECT_EQ(a.mgr->test_punch_stats().attempts, 0u);
    EXPECT_EQ(b.mgr->test_punch_stats().received, 0u) << "a peer without NODE_HOLEPUNCH got punch messages";
}

// Review Focus 5: the acceptor's address went stale since its last STUN round.
TEST(HolePunchManager, StaleCandidateTimesOutAndBacksOff) {
    Node a, b;
    a.Listen();
    b.Listen();
    b.mgr->test_set_local_punch_candidates(Cands(25905));  // nothing listens there
    Leg leg(25031, 25032);
    const auto [key_a, key_b] = Relay(a, b, leg, 0x501);

    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return a.mgr->test_punch_stats().dials == 1; }));
    for (int i = 0; i < 10; ++i) { TickAll({&a, &b}); std::this_thread::sleep_for(50ms); }
    EXPECT_EQ(a.mgr->test_punch_stats().timeout, 0u) << "timed out before the deadline";

    a.Advance(10s + 1ms);
    b.Advance(10s + 1ms);
    TickAll({&a, &b});
    EXPECT_EQ(a.mgr->test_punch_stats().timeout, 1u);
    EXPECT_EQ(b.mgr->test_punch_stats().timeout, 1u);
    EXPECT_TRUE(Live(*a.mgr, key_a)) << "a failed punch dropped the relayed peer";
    EXPECT_TRUE(Live(*b.mgr, key_b));
    EXPECT_FALSE(Live(*a.mgr, Key6(25905))) << "the dead punched dial was left behind";

    a.Advance(29min);
    b.Advance(29min);
    TickAll({&a, &b});
    EXPECT_EQ(a.mgr->test_punch_stats().offers_sent, 1u) << "re-offered inside the 30 min backoff";

    a.Advance(2min);
    b.Advance(2min);
    TickAll({&a, &b});
    EXPECT_EQ(a.mgr->test_punch_stats().offers_sent, 2u) << "no new attempt after the backoff";
}

// Review Focus 4 (and the no-inbound-slot case): B refuses A's Initial.
TEST(HolePunchManager, AcceptorWithoutInboundSlotFailsCleanly) {
    Node a, b;
    a.Listen();
    const uint16_t bp = b.Listen();
    b.mgr->test_set_inbound_limits(/*total=*/1, /*per_ip=*/12);  // the relayed peer fills it
    Leg leg(25041, 25042);
    const auto [key_a, key_b] = Relay(a, b, leg, 0x601);

    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return a.mgr->test_punch_stats().dials == 1; }));
    for (int i = 0; i < 40; ++i) { TickAll({&a, &b}); std::this_thread::sleep_for(50ms); }
    EXPECT_FALSE(Proven(*a.mgr, Key6(bp))) << "B accepted a punched peer with no inbound slot";

    a.Advance(10s + 1ms);
    b.Advance(10s + 1ms);
    TickAll({&a, &b});
    for (int i = 0; i < 10; ++i) { TickAll({&a, &b}); std::this_thread::sleep_for(20ms); }
    const auto sa = a.mgr->test_punch_stats();
    EXPECT_EQ(sa.timeout, 1u);
    EXPECT_EQ(sa.attempts, 1u);
    EXPECT_EQ(sa.offers_sent, 1u);
    EXPECT_EQ(sa.dials, 1u);
    EXPECT_TRUE(Live(*a.mgr, key_a));
    EXPECT_TRUE(Live(*b.mgr, key_b));
}

// Review Focus 4: a relayed peer names third parties as its candidates.
TEST(HolePunchManager, AcceptorBoundsProbesToClaimedTargets) {
    Node b;
    b.Listen();
    const auto evil = FakeId(0x01);  // lower than any real id: B is the acceptor anyway (inbound only)
    const auto key = FakeRelayedPeer(*b.mgr, evil, "relay:evil:7");

    PunchCandidates claimed;
    for (uint16_t i = 0; i < 8; ++i) claimed.v6.push_back(Loop6(static_cast<uint16_t>(25910 + i)));
    b.mgr->test_process_message(key, P2PMessage::create_punch_offer(42, claimed));
    ASSERT_EQ(b.mgr->test_punch_stats().answers_sent, 1u);
    b.mgr->test_process_message(key, P2PMessage::create_punch_go(42));

    for (int i = 0; i < 200; ++i) {  // 20 s of punch time, ticked every 100 ms
        b.Advance(100ms);
        TickAll({&b});
    }
    const auto s = b.mgr->test_punch_stats();
    EXPECT_GT(s.probes_sent, 0u);
    EXPECT_LE(s.probes_sent, 3u * 40u);
    EXPECT_LE(s.max_probe_targets, 3u);
    EXPECT_EQ(s.max_probe_targets, 3u);
    EXPECT_EQ(s.timeout, 1u);

    b.mgr->test_process_message(key, P2PMessage::create_punch_offer(43, claimed));
    EXPECT_EQ(b.mgr->test_punch_stats().answers_sent, 1u) << "answered again inside the backoff";

    b.Advance(31min);
    b.mgr->test_process_message(key, P2PMessage::create_punch_offer(44, claimed));
    EXPECT_EQ(b.mgr->test_punch_stats().answers_sent, 2u) << "still refused after the backoff";
}

// Review Focus 1: a busy home node with every direct slot full.
TEST(HolePunchManager, InitiatorAtFullOutboundStillPunches) {
    Node a, b;
    a.Listen();
    const uint16_t bp = b.Listen();
    for (int i = 0; i < 8; ++i) {
        a.mgr->test_install_connected_direct_peer("44.55.66." + std::to_string(i + 1) + ":20999", -1,
                                                  true, true, FakeId(static_cast<uint8_t>(0x40 + i)));
    }
    Leg leg(25051, 25052);
    Relay(a, b, leg, 0x701);
    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return Proven(*a.mgr, Key6(bp)); }))
        << "a full direct-outbound set blocked the punch";
    EXPECT_TRUE(a.mgr->get_peer_info(Key6(bp))->punched);
}

// A dead first target (stale or firewalled IPv6 silently drops the Initial)
// must not use up the whole attempt: each target gets its share of the window,
// then the next one is dialed.
TEST(HolePunchManager, DeadFirstTargetFallsBackToNext) {
    Node a, b;
    a.Listen();
    const uint16_t bp = b.Listen();
    PunchCandidates two;
    two.v6.push_back(Loop6(25930));  // nothing listens here
    two.v6.push_back(Loop6(bp));
    b.mgr->test_set_local_punch_candidates(two);
    Leg leg(25061, 25062);
    Relay(a, b, leg, 0x801);

    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return a.mgr->test_punch_stats().dials == 1; }));
    // Half the 10 s window has passed for the first of two targets.
    a.Advance(5s + 100ms);
    b.Advance(5s + 100ms);
    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return Proven(*a.mgr, Key6(bp)); }, 4))
        << "the second target was never dialed inside the attempt deadline";
    EXPECT_TRUE(a.mgr->get_peer_info(Key6(bp))->punched);
    const auto s = a.mgr->test_punch_stats();
    EXPECT_EQ(s.dials, 2u);
    EXPECT_EQ(s.timeout, 0u);
    EXPECT_FALSE(Live(*a.mgr, Key6(25930))) << "the dead first dial was left behind";
}

TEST(HolePunchManager, UnsolicitedPunchMessagesIgnored) {
    Node b;
    b.Listen();
    // A direct TCP peer, proven, with the bit: not a relayed link.
    b.mgr->test_install_connected_direct_peer("203.0.113.5:20999", -1, false, true, FakeId(0x02));
    b.mgr->test_set_peer_services("203.0.113.5:20999", dinero::ServiceFlags::NODE_HOLEPUNCH);
    b.mgr->test_process_message("203.0.113.5:20999", P2PMessage::create_punch_offer(5, Cands(25920)));
    auto s = b.mgr->test_punch_stats();
    EXPECT_EQ(s.answers_sent, 0u);
    EXPECT_EQ(s.attempts, 0u);
    EXPECT_EQ(s.ignored, 1u);

    // A relayed peer with an attempt in flight, then punchgo with the wrong nonce.
    const auto key = FakeRelayedPeer(*b.mgr, FakeId(0x03), "relay:fake:7");
    b.mgr->test_process_message(key, P2PMessage::create_punch_offer(6, Cands(25921)));
    ASSERT_EQ(b.mgr->test_punch_stats().answers_sent, 1u);
    b.mgr->test_process_message(key, P2PMessage::create_punch_go(7));
    b.mgr->test_process_message(key, P2PMessage::create_punch_answer(6, Cands(25921)));  // we never offered
    b.mgr->test_process_message(key, P2PMessage::create_punch_offer(8, Cands(25921)));   // duplicate
    for (int i = 0; i < 5; ++i) { b.Advance(250ms); TickAll({&b}); }
    s = b.mgr->test_punch_stats();
    EXPECT_EQ(s.probes_sent, 0u) << "a wrong-nonce punchgo started probing";
    EXPECT_EQ(s.attempts, 1u);
    EXPECT_EQ(s.answers_sent, 1u);
    EXPECT_EQ(s.ignored, 4u);
}

// Plan 4 Task 4 gap: a punched outbound peer (not via_relay) uses the
// home-peer budget, so with one relayed and one punched peer a third home
// peer is not dialed even though the direct slots are full.
namespace {
std::unique_ptr<P2PMessage> ReadRelayConnect(int fd) {
    timeval tv{0, 200 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (int i = 0; i < 8; ++i) {
        std::vector<uint8_t> frame(24);
        size_t got = 0;
        while (got < frame.size()) {
            const auto n = ::recv(fd, frame.data() + got, frame.size() - got, 0);
            if (n <= 0) return nullptr;
            got += static_cast<size_t>(n);
        }
        uint32_t len = 0;
        for (int k = 0; k < 4; ++k) len |= static_cast<uint32_t>(frame[16 + k]) << (k * 8);
        frame.resize(24 + len);
        while (got < frame.size()) {
            const auto n = ::recv(fd, frame.data() + got, frame.size() - got, 0);
            if (n <= 0) return nullptr;
            got += static_cast<size_t>(n);
        }
        auto m = P2PMessage::deserialize(frame);
        if (!m || m->command == "relaycon") return m;
    }
    return nullptr;
}
}  // namespace

TEST(HolePunchManager, PunchedPeerCountsAsHomePeer) {
    Node a, b;
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t bp = b.mgr->quic_endpoint()->Port();
    int relay[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, relay), 0);
    a.mgr->test_install_connected_direct_peer("9.9.20.2:20999", relay[0], true, false, {});
    for (int i = 0; i < 7; ++i) {
        a.mgr->test_install_connected_direct_peer("44.55.66." + std::to_string(i + 1) + ":20999", -1,
                                                  true, false, {});
    }
    a.mgr->test_install_virtual_relay_peer("relay:home0", "9.9.20.2:20999", 100,
                                           P2PMessage::RelayDirection::ClientToTarget, true);
    ASSERT_TRUE(a.mgr->connect_to_peer_quic("127.0.0.1", bp, b.NodeId()));
    const std::string punched = "127.0.0.1:" + std::to_string(bp);
    ASSERT_TRUE(WaitProven(*a.mgr, punched));
    ASSERT_TRUE(a.mgr->get_peer_info(punched)->punched);
    ASSERT_FALSE(a.mgr->get_peer_info(punched)->via_relay.has_value());

    auto target = MakeIdentity();
    P2PMessage::RelayHint h;
    h.target_node_id = target->get_node_id_bytes();
    h.relay_net = dinero::p2p::NetworkType::IPV4;
    h.relay_addr = {9, 9, 20, 2};
    h.relay_port = 20999;
    a.mgr->handle_relay_hints("10.4.0.9:20999", P2PMessage::create_relay_hints({h}));
    a.mgr->test_orchestrate_relay_dials();
    EXPECT_EQ(ReadRelayConnect(relay[1]), nullptr)
        << "a relayed + a punched peer left room for a third home peer";
    ::close(relay[1]);
}

// ---- Task 6: prove, replace and report ----

namespace {
// Connected, identity-proven peers for one node id, from the snapshot
// getpeerinfo reads.
std::vector<PeerInfo> PeersFor(P2PManager& m, const std::array<uint8_t, 20>& id) {
    std::vector<PeerInfo> out;
    for (auto& p : m.get_connected_peers()) {
        if (p.identity_proven && p.their_node_id == id) out.push_back(std::move(p));
    }
    return out;
}

std::chrono::system_clock::time_point LastMessageAt(P2PManager& m, const std::string& key) {
    auto* p = m.get_peer_info(key);
    return p ? p->last_message_at : std::chrono::system_clock::time_point{};
}
}  // namespace

TEST(HolePunchManager, SuccessReplacesRelayedPeer) {
    Node a, b;
    a.Listen();
    const uint16_t bp = b.Listen();
    Leg leg(25071, 25072);
    const auto [key_a, key_b] = Relay(a, b, leg, 0x901);

    ASSERT_TRUE(TickUntil({&a, &b}, [&] {
        return a.mgr->test_punch_stats().succeeded == 1 && b.mgr->test_punch_stats().succeeded == 1;
    })) << "the punch never succeeded on both sides";
    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return !Live(*a.mgr, key_a) && !Live(*b.mgr, key_b); }, 5))
        << "the relayed peer survived a successful punch";

    const auto pa = PeersFor(*a.mgr, b.NodeId());
    ASSERT_EQ(pa.size(), 1u) << "A does not have exactly one peer for B";
    EXPECT_EQ(pa[0].to_string(), Key6(bp));
    EXPECT_STREQ(pa[0].path_name(), "punched");
    EXPECT_STREQ(pa[0].transport_name(), "quic");
    const auto pb = PeersFor(*b.mgr, a.NodeId());
    ASSERT_EQ(pb.size(), 1u) << "B does not have exactly one peer for A";
    EXPECT_TRUE(pb[0].quic_direct);
    EXPECT_FALSE(pb[0].is_outbound);
    EXPECT_STREQ(pb[0].path_name(), "punched");
    const std::string key_ba = pb[0].to_string();
    for (const auto& s : {a.mgr->test_punch_stats(), b.mgr->test_punch_stats()}) {
        EXPECT_EQ(s.ipv6.succeeded, 1u);
        EXPECT_EQ(s.ipv4.succeeded, 0u);
    }

    // A ping over the punched session reaches B, and B's pong reaches A.
    // No keepalive runs here, so wait for the post-handshake chatter to settle.
    for (int i = 0; i < 20; ++i) {
        const auto ta = LastMessageAt(*a.mgr, Key6(bp)), tb = LastMessageAt(*b.mgr, key_ba);
        std::this_thread::sleep_for(300ms);
        if (ta == LastMessageAt(*a.mgr, Key6(bp)) && tb == LastMessageAt(*b.mgr, key_ba)) break;
    }
    const auto sent_at = std::chrono::system_clock::now();
    ASSERT_TRUE(a.mgr->send_to_peer(Key6(bp), P2PMessage::create_ping(0x5150)));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (LastMessageAt(*b.mgr, key_ba) < sent_at || LastMessageAt(*a.mgr, Key6(bp)) < sent_at)) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_GE(LastMessageAt(*b.mgr, key_ba).time_since_epoch().count(), sent_at.time_since_epoch().count()) << "the ping never reached B";
    EXPECT_GE(LastMessageAt(*a.mgr, Key6(bp)).time_since_epoch().count(), sent_at.time_since_epoch().count()) << "the pong never came back to A";
}

// Review Focus 3: a third node C answers at the address B claimed.
TEST(HolePunchManager, WrongIdentityAtPunchedAddressIsDropped) {
    Node a, b, c;
    a.Listen();
    b.Listen();
    const uint16_t cp = c.Listen();
    b.mgr->test_set_local_punch_candidates(Cands(cp));
    Leg leg(25081, 25082);
    const auto [key_a, key_b] = Relay(a, b, leg, 0xA01);

    ASSERT_TRUE(TickUntil({&a, &b, &c}, [&] { return a.mgr->test_punch_stats().mismatch == 1; }))
        << "C's identity at the punched address was not counted as a mismatch";
    ASSERT_TRUE(TickUntil({&a, &b, &c}, [&] { return !Live(*a.mgr, Key6(cp)); }, 5))
        << "the punched session to C was not disconnected";
    EXPECT_TRUE(PeersFor(*a.mgr, c.NodeId()).empty());
    EXPECT_TRUE(Live(*a.mgr, key_a)) << "the relayed peer to B was dropped";
    EXPECT_TRUE(Live(*b.mgr, key_b));
    const auto pa = PeersFor(*a.mgr, b.NodeId());
    ASSERT_EQ(pa.size(), 1u);
    EXPECT_STREQ(pa[0].path_name(), "relayed");

    // The mismatch ended the attempt: the deadline adds no timeout.
    a.Advance(10s + 1ms);
    b.Advance(10s + 1ms);
    TickAll({&a, &b, &c});
    const auto s = a.mgr->test_punch_stats();
    EXPECT_EQ(s.mismatch, 1u);
    EXPECT_EQ(s.timeout, 0u);
    EXPECT_EQ(s.succeeded, 0u);
    EXPECT_TRUE(Live(*a.mgr, key_a));
}

TEST(HolePunchManager, NonBoundProofOnPunchedSessionIsRefused) {
    Node a, b;
    a.Listen();
    const uint16_t bp = b.Listen();
    Leg leg(25091, 25092);
    const auto [key_a, key_b] = Relay(a, b, leg, 0xB01);
    b.mgr->test_set_dineroid_v1_only(true);  // only after the relayed link is proven

    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return a.mgr->test_punch_stats().refused == 1; }))
        << "a v1-only proof on the punched session was not refused";
    ASSERT_TRUE(TickUntil({&a, &b}, [&] { return !Live(*a.mgr, Key6(bp)); }, 5))
        << "the punched session with a v1-only proof stayed up";
    EXPECT_FALSE(Proven(*a.mgr, Key6(bp)));
    EXPECT_TRUE(Live(*a.mgr, key_a));
    EXPECT_TRUE(Live(*b.mgr, key_b));
    a.Advance(10s + 1ms);
    TickAll({&a});
    EXPECT_EQ(a.mgr->test_punch_stats().succeeded, 0u);
    EXPECT_EQ(a.mgr->test_punch_stats().timeout, 0u);
}

// A succeeds with B over IPv6 and times out with C over IPv4.
TEST(HolePunchManager, NetworkInfoCountsOutcomes) {
    Node a, b, c;
    const uint16_t ap = a.Listen();
    const uint16_t bp = b.Listen();
    PunchCandidates al = Cands(ap);
    const uint8_t lo2[4] = {127, 0, 0, 2};
    al.v4 = UdpAddr::FromIPv4(lo2, ap);
    a.mgr->test_set_local_punch_candidates(al);
    PunchCandidates cl;
    cl.v4 = Addr4(25941);  // nothing listens there
    c.mgr->test_set_local_punch_candidates(cl);
    Leg ab(25101, 25102), ac(25103, 25104);
    Relay(a, b, ab, 0xC01);
    Relay(a, c, ac, 0xD01);

    ASSERT_TRUE(TickUntil({&a, &b, &c}, [&] {
        const auto s = a.mgr->test_punch_stats();
        return s.succeeded == 1 && s.dials == 2;
    })) << "A never punched B and dialed C";
    EXPECT_TRUE(Proven(*a.mgr, Key6(bp)));
    a.Advance(10s + 1ms);
    c.Advance(10s + 1ms);
    TickAll({&a, &b, &c});

    const auto s = a.mgr->test_punch_stats();
    EXPECT_EQ(s.attempts, 2u);
    EXPECT_EQ(s.succeeded, 1u);
    EXPECT_EQ(s.timeout, 1u);
    EXPECT_EQ(s.mismatch, 0u);
    EXPECT_EQ(s.refused, 0u);
    EXPECT_EQ(s.ipv6.attempts, 1u);
    EXPECT_EQ(s.ipv6.succeeded, 1u);
    EXPECT_EQ(s.ipv4.attempts, 1u);
    EXPECT_EQ(s.ipv4.succeeded, 0u);
    const auto sb = b.mgr->test_punch_stats();
    EXPECT_EQ(sb.succeeded, 1u);
    EXPECT_EQ(sb.ipv6.attempts, 1u);
    EXPECT_EQ(sb.ipv6.succeeded, 1u);
    EXPECT_EQ(sb.ipv4.attempts, 0u);
    const auto sc = c.mgr->test_punch_stats();
    EXPECT_EQ(sc.timeout, 1u);
    EXPECT_EQ(sc.ipv4.attempts, 1u);
    EXPECT_EQ(sc.ipv4.succeeded, 0u);
    EXPECT_EQ(sc.ipv6.attempts, 0u);
}
