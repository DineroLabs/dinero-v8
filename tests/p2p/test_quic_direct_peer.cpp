// Direct-QUIC peers: a normal P2P peer whose transport is a QuicSession on a
// real UDP path instead of a TCP socket. These cases drive two real
// P2PManagers through the full version/verack + dineroid handshake over QUIC.

#include "p2p_manager.h"
#include "daemon/node_identity.h"
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
