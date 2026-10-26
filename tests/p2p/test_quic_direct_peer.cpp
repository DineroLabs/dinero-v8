// Direct-QUIC peers: a normal P2P peer whose transport is a QuicSession on a
// real UDP path instead of a TCP socket. These cases drive two real
// P2PManagers through the full version/verack + dineroid handshake over QUIC.

#include "p2p_manager.h"
#include "daemon/node_identity.h"
#include "network/quic_endpoint.h"
#include "network/quic_session.h"
#include "network/relay_tls_keypair.h"
#include "network/types.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using dinero::network::QuicSession;
using dinero::network::QuicSessionOptions;
using dinero::network::UdpAddr;

QuicSessionOptions P2pOpts() {
    static std::string cert, key;
    static std::once_flag once;
    std::call_once(once, [] { ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert, &key, nullptr)); });
    QuicSessionOptions o;
    o.alpn = "dinero-p2p/1";
    o.certificate_pem = cert;
    o.private_key_pem = key;
    return o;
}

UdpAddr Loopback(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return UdpAddr::FromIPv4(ip, port);
}

// Two shared sessions whose writers feed each other (no sockets).
struct SharedLoopback {
    struct Route { std::mutex mu; QuicSession* a{nullptr}; QuicSession* b{nullptr}; };
    std::shared_ptr<Route> route = std::make_shared<Route>();
    std::shared_ptr<QuicSession> client, server;
    SharedLoopback() {
        client = std::make_shared<QuicSession>([r = route](std::vector<uint8_t> x) {
            std::lock_guard<std::mutex> l(r->mu); if (r->b) r->b->EnqueueIncomingPacket(std::move(x)); });
        server = std::make_shared<QuicSession>([r = route](std::vector<uint8_t> x) {
            std::lock_guard<std::mutex> l(r->mu); if (r->a) r->a->EnqueueIncomingPacket(std::move(x)); });
        std::lock_guard<std::mutex> l(route->mu);
        route->a = client.get();
        route->b = server.get();
    }
    void Disconnect() { std::lock_guard<std::mutex> l(route->mu); route->a = route->b = nullptr; }
};

struct Node {
    std::filesystem::path dir;
    std::unique_ptr<P2PManager> mgr;
    explicit Node(const char* name) {
        dir = std::filesystem::temp_directory_path() / ("quic-direct-" + std::string(name) + "-" +
              std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        mgr = std::make_unique<P2PManager>(0);
        auto id = std::make_shared<dinero::daemon::NodeIdentity>();
        EXPECT_TRUE(id->initialize(dir.string()));
        mgr->set_node_identity(id);
        mgr->set_service_flags_provider([] {
            return dinero::ServiceFlags::NODE_NETWORK | dinero::ServiceFlags::NODE_DINERO_V2 |
                   dinero::ServiceFlags::NODE_COMPACT_TIMING_V1;
        });
        mgr->set_height_provider([] { return 10u; });
    }
    ~Node() {
        mgr->test_set_running(true);
        mgr->stop();
        mgr.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

std::string Hex(const std::array<uint8_t, 20>& id) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto b : id) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

bool WaitConnected(P2PManager& m, const std::string& key, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* p = m.get_peer_info(key); p && p->is_connected && p->identity_proven) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

TEST(QuicDirectPeer, TwoManagersHandshakeOverQuicPair) {
    Node a("a"), b("b");
    SharedLoopback link;
    const auto a_addr = Loopback(23001), b_addr = Loopback(23002);
    ASSERT_TRUE(link.server->StartServer(b_addr, a_addr, P2pOpts()));
    ASSERT_TRUE(link.client->StartClient(a_addr, b_addr, P2pOpts()));

    const auto key_on_a = a.mgr->install_direct_quic_peer(link.client, b_addr, /*outbound=*/true);
    const auto key_on_b = b.mgr->install_direct_quic_peer(link.server, a_addr, /*outbound=*/false);
    EXPECT_EQ(key_on_a, "127.0.0.1:23002");
    EXPECT_EQ(key_on_b, "127.0.0.1:23001");

    ASSERT_TRUE(WaitConnected(*a.mgr, key_on_a)) << "A never completed the QUIC peer handshake";
    ASSERT_TRUE(WaitConnected(*b.mgr, key_on_b)) << "B never completed the QUIC peer handshake";
    EXPECT_EQ(Hex(a.mgr->get_peer_info(key_on_a)->their_node_id), b.mgr->get_local_node_id_hex());
    EXPECT_EQ(Hex(b.mgr->get_peer_info(key_on_b)->their_node_id), a.mgr->get_local_node_id_hex());
    EXPECT_TRUE(a.mgr->get_peer_info(key_on_a)->quic_direct);
    EXPECT_TRUE(a.mgr->get_peer_info(key_on_a)->is_outbound);
    EXPECT_FALSE(b.mgr->get_peer_info(key_on_b)->is_outbound);

    // After the handshake, ordinary messages flow through send_to_peer: an
    // unknown command reaches B's pass-through handler.
    std::mutex mu;
    std::string got_from;
    b.mgr->set_message_handler([&](const std::string& peer, const P2PMessage& m) {
        if (m.command == "quictest") { std::lock_guard<std::mutex> l(mu); got_from = peer; }
    });
    P2PMessage probe;
    probe.command = "quictest";
    probe.payload = {1, 2, 3};
    ASSERT_TRUE(a.mgr->send_to_peer(key_on_a, probe));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        { std::lock_guard<std::mutex> l(mu); if (!got_from.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::lock_guard<std::mutex> l(mu);
    EXPECT_EQ(got_from, key_on_b);
    link.Disconnect();
}

// stop() must wake a QUIC peer's handler, which sleeps on its inbox (not a
// socket), or daemon shutdown waits out the 10 s idle receive per QUIC peer.
TEST(QuicDirectPeer, StopDoesNotWaitOnIdleQuicPeer) {
    Node a("stop-a"), b("stop-b");
    SharedLoopback link;
    const auto a_addr = Loopback(23011), b_addr = Loopback(23012);
    ASSERT_TRUE(link.server->StartServer(b_addr, a_addr, P2pOpts()));
    ASSERT_TRUE(link.client->StartClient(a_addr, b_addr, P2pOpts()));
    const auto key_on_a = a.mgr->install_direct_quic_peer(link.client, b_addr, true);
    const auto key_on_b = b.mgr->install_direct_quic_peer(link.server, a_addr, false);
    ASSERT_TRUE(WaitConnected(*a.mgr, key_on_a));
    ASSERT_TRUE(WaitConnected(*b.mgr, key_on_b));

    a.mgr->test_set_running(true);
    const auto start = std::chrono::steady_clock::now();
    a.mgr->stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(3))
        << "stop() took " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << " ms with one idle QUIC peer";
    link.Disconnect();
}

// ---- Task 4: real UDP dial/accept through each manager's QuicEndpoint ----

namespace {

std::string LoopKey(uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

size_t PeerCount(P2PManager& m) {
    size_t n = 0;
    for (const auto& p : m.get_connected_peers()) if (p.is_connected) ++n;
    return n;
}

}  // namespace

TEST(QuicPeer, DialOverRealUdpReachesConnected) {
    Node a("dial-a"), b("dial-b");
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t ap = a.mgr->quic_endpoint()->Port(), bp = b.mgr->quic_endpoint()->Port();
    ASSERT_TRUE(a.mgr->connect_to_peer_quic("127.0.0.1", bp));
    ASSERT_TRUE(WaitConnected(*a.mgr, LoopKey(bp)));
    ASSERT_TRUE(WaitConnected(*b.mgr, LoopKey(ap)));
    EXPECT_TRUE(a.mgr->get_peer_info(LoopKey(bp))->is_outbound);
    EXPECT_FALSE(b.mgr->get_peer_info(LoopKey(ap))->is_outbound);
    EXPECT_TRUE(b.mgr->get_peer_info(LoopKey(ap))->quic_direct);
}

TEST(QuicPeer, DuplicateTransportToSameAddressIsRejected) {
    Node a("dup-a"), b("dup-b");
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t ap = a.mgr->quic_endpoint()->Port(), bp = b.mgr->quic_endpoint()->Port();

    // Dial side: an existing (TCP) peer at ip:port means no second session.
    a.mgr->test_install_connected_direct_peer(LoopKey(bp), -1, true, false, {});
    EXPECT_FALSE(a.mgr->connect_to_peer_quic("127.0.0.1", bp));
    a.mgr->test_cleanup_peer(LoopKey(bp));

    // Accept side: B already has a peer at A's address, so B refuses A's Initial.
    b.mgr->test_install_connected_direct_peer(LoopKey(ap), -1, false, false, {});
    ASSERT_TRUE(a.mgr->connect_to_peer_quic("127.0.0.1", bp));
    EXPECT_FALSE(WaitConnected(*a.mgr, LoopKey(bp), 5));
    EXPECT_EQ(b.mgr->quic_endpoint()->SessionCount(), 0u);
}

TEST(QuicPeer, InboundLimitAppliesToQuic) {
    Node a1("lim-a1"), a2("lim-a2"), b("lim-b");
    b.mgr->test_set_inbound_limits(/*total=*/1, /*per_ip=*/12);
    ASSERT_TRUE(a1.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(a2.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t bp = b.mgr->quic_endpoint()->Port();
    ASSERT_TRUE(a1.mgr->connect_to_peer_quic("127.0.0.1", bp));
    ASSERT_TRUE(WaitConnected(*a1.mgr, LoopKey(bp)));
    ASSERT_TRUE(a2.mgr->connect_to_peer_quic("127.0.0.1", bp));
    EXPECT_FALSE(WaitConnected(*a2.mgr, LoopKey(bp), 5)) << "second inbound QUIC peer exceeded cap 1";
    EXPECT_EQ(PeerCount(*b.mgr), 1u);
}

TEST(QuicPeer, BannedIpCannotConnectOverQuic) {
    Node a("ban-a"), b("ban-b");
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->ban_peer("127.0.0.1", std::chrono::seconds(600)));
    const uint16_t bp = b.mgr->quic_endpoint()->Port();
    ASSERT_TRUE(a.mgr->connect_to_peer_quic("127.0.0.1", bp));
    EXPECT_FALSE(WaitConnected(*a.mgr, LoopKey(bp), 5));
    EXPECT_EQ(b.mgr->quic_endpoint()->SessionCount(), 0u);
}

// A peer that vanishes without a CONNECTION_CLOSE (sleep, NAT drop) must be
// noticed by QUIC's idle timeout and cleaned up, freeing its slot.
TEST(QuicPeer, SilentRemoteIsReapedAndSlotFreed) {
    Node a("silent-a"), b("silent-b");
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t ap = a.mgr->quic_endpoint()->Port(), bp = b.mgr->quic_endpoint()->Port();
    ASSERT_TRUE(a.mgr->connect_to_peer_quic("127.0.0.1", bp));
    ASSERT_TRUE(WaitConnected(*b.mgr, LoopKey(ap)));

    a.mgr->quic_endpoint()->Stop();  // marks the socket dead first: nothing more reaches B

    // Worst case is keep-alive (20 s) + idle timeout (60 s): QUIC restarts the
    // idle timer on the first ack-eliciting packet sent after the last receipt.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        auto* p = b.mgr->get_peer_info(LoopKey(ap));
        if ((!p || !p->is_connected) && b.mgr->quic_endpoint()->SessionCount() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    auto* p = b.mgr->get_peer_info(LoopKey(ap));
    std::string state = "peer gone";
    if (p && p->relay_quic_session) {
        state = std::string("session active=") + (p->relay_quic_session->active() ? "1" : "0") +
                " ready=" + (p->relay_quic_session->handshake_ready() ? "1" : "0") +
                " err=" + p->relay_quic_session->last_error();
    }
    EXPECT_TRUE(!p || !p->is_connected) << "vanished QUIC peer still connected after 90 s; " << state;
    EXPECT_EQ(b.mgr->quic_endpoint()->SessionCount(), 0u) << "endpoint still routes to the dead session";
}

// ---- Task 5: transport visibility, service bit, config ----

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
uint16_t FreeTcpUdpPort() {
    for (int attempt = 0; attempt < 20; ++attempt) {
        int t = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a); ::getsockname(t, reinterpret_cast<sockaddr*>(&a), &len);
        const uint16_t port = ntohs(a.sin_port);
        ::close(t);
        int u = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in b{}; b.sin_family = AF_INET; b.sin_port = htons(port); b.sin_addr.s_addr = htonl(INADDR_ANY);
        const bool udp_free = ::bind(u, reinterpret_cast<sockaddr*>(&b), sizeof(b)) == 0;
        ::close(u);
        if (udp_free) return port;
    }
    return 0;
}
bool UdpPortFree(uint16_t port) {
    int u = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in b{}; b.sin_family = AF_INET; b.sin_port = htons(port); b.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool ok = ::bind(u, reinterpret_cast<sockaddr*>(&b), sizeof(b)) == 0;
    ::close(u);
    return ok;
}
}  // namespace

TEST(PeerJson, TransportFieldReflectsPeerKind) {
    PeerInfo tcp; tcp.socket_fd = 5;
    EXPECT_STREQ(tcp.transport_name(), "tcp");
    PeerInfo relay; relay.via_relay = PeerInfo::ViaRelayInfo{1, "1.2.3.4:20999", 0};
    EXPECT_STREQ(relay.transport_name(), "relay");
    PeerInfo quic; quic.quic_direct = true;
    EXPECT_STREQ(quic.transport_name(), "quic");
}

TEST(QuicPeer, ConnectedPeersSnapshotKeepsQuicTransport) {
    Node a("snap-a"), b("snap-b");
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t bp = b.mgr->quic_endpoint()->Port();
    ASSERT_TRUE(a.mgr->connect_to_peer_quic("127.0.0.1", bp));
    ASSERT_TRUE(WaitConnected(*a.mgr, LoopKey(bp)));
    bool found = false;
    for (const auto& p : a.mgr->get_connected_peers()) {
        if (p.to_string() == LoopKey(bp)) { found = true; EXPECT_STREQ(p.transport_name(), "quic"); }
    }
    EXPECT_TRUE(found);
}

TEST(QuicPeerConfig, DisabledEndpointDoesNotAdvertiseOrBind) {
    const uint16_t port = FreeTcpUdpPort();
    ASSERT_NE(port, 0);
    P2PManager mgr(port);
    mgr.set_quic_enabled(false);
    mgr.set_quic_connect_targets({"127.0.0.1:1"});
    ASSERT_TRUE(mgr.start());
    EXPECT_EQ(mgr.quic_endpoint(), nullptr);
    EXPECT_EQ(mgr.local_quic_service_bits(), 0u);
    EXPECT_TRUE(UdpPortFree(port)) << "UDP " << port << " bound although p2p.quic=0";
    mgr.dial_quic_connect_targets();  // must warn and do nothing, not crash
    EXPECT_EQ(mgr.quic_endpoint(), nullptr);
    mgr.stop();
}

TEST(QuicPeerConfig, EnabledEndpointAdvertisesServiceBit) {
    Node a("bit-a");
    EXPECT_EQ(a.mgr->local_quic_service_bits(), 0u);
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    EXPECT_EQ(a.mgr->local_quic_service_bits(), dinero::ServiceFlags::NODE_P2P_QUIC);
}

TEST(QuicPeerConfig, QuicConnectTargetsAreDialed) {
    Node a("tgt-a"), b("tgt-b");
    ASSERT_TRUE(a.mgr->start_quic_endpoint(0));
    ASSERT_TRUE(b.mgr->start_quic_endpoint(0));
    const uint16_t bp = b.mgr->quic_endpoint()->Port();
    a.mgr->set_quic_connect_targets({LoopKey(bp)});
    a.mgr->dial_quic_connect_targets();
    EXPECT_TRUE(WaitConnected(*a.mgr, LoopKey(bp)));
    a.mgr->dial_quic_connect_targets();  // already connected: no second session
    EXPECT_EQ(a.mgr->quic_endpoint()->SessionCount(), 1u);
}
