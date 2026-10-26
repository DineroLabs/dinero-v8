// Relayed QUIC sessions carry no TLS certificate check, so a relay could
// terminate both legs and pass each side's `dineroid` to the other. dineroid
// v2 signs nonce || the session's TLS exporter value instead: the two legs of
// a man in the middle have different exporter values, so both ends notice.
// Plus: an outbound relayed dial must reach the node it asked for.

#include "p2p_manager.h"
#include "daemon/node_identity.h"
#include "network/quic_session.h"
#include "network/relay_tls_keypair.h"
#include "network/types.h"
#include "p2p/relay_hint_v2.h"
#include "quic_loopback_pair.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using dinero::network::QuicSession;
using dinero::network::QuicSessionOptions;
using dinero::network::UdpAddr;

QuicSessionOptions Opts() {
    static std::string cert, key;
    static std::once_flag once;
    std::call_once(once, [] {
        ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert, &key, nullptr));
    });
    QuicSessionOptions o;
    o.certificate_pem = cert;
    o.private_key_pem = key;
    return o;
}

UdpAddr Addr(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return UdpAddr::FromIPv4(ip, port);
}

// Two shared sessions whose writers feed each other.
struct Leg {
    struct Route {
        std::mutex mu;
        QuicSession* a{nullptr};
        QuicSession* b{nullptr};
    };
    std::shared_ptr<Route> route = std::make_shared<Route>();
    std::shared_ptr<QuicSession> client, server;
    Leg(uint16_t client_port, uint16_t server_port) {
        client = std::make_shared<QuicSession>([r = route](std::vector<uint8_t> x) {
            std::lock_guard<std::mutex> l(r->mu);
            if (r->b) r->b->EnqueueIncomingPacket(std::move(x));
        });
        server = std::make_shared<QuicSession>([r = route](std::vector<uint8_t> x) {
            std::lock_guard<std::mutex> l(r->mu);
            if (r->a) r->a->EnqueueIncomingPacket(std::move(x));
        });
        {
            std::lock_guard<std::mutex> l(route->mu);
            route->a = client.get();
            route->b = server.get();
        }
        EXPECT_TRUE(server->StartServer(Addr(server_port), Addr(client_port), Opts()));
        EXPECT_TRUE(client->StartClient(Addr(client_port), Addr(server_port), Opts()));
    }
    ~Leg() {
        std::lock_guard<std::mutex> l(route->mu);
        route->a = route->b = nullptr;
    }
};

std::shared_ptr<dinero::daemon::NodeIdentity> MakeIdentity() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero-binding-" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
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
    explicit Node(uint16_t port = 0) {
        mgr = std::make_unique<P2PManager>(port);
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
    }
};

bool WaitProven(P2PManager& m, const std::string& key, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* p = m.get_peer_info(key); p && p->is_connected && p->identity_proven) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// True once the peer's handler gave up on it (cleanup leaves it STOPPING).
bool WaitDropped(P2PManager& m, const std::string& key, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        auto* p = m.get_peer_info(key);
        if (!p || p->lifetime_state.load() != PeerLifetimeState::RUNNING) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool WaitReady(QuicSession& s) {
    return s.WaitHandshakeReady().wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
           s.WaitHandshakeReady().get();
}

}  // namespace

TEST(RelayIdentityBinding, BindingMatchesOnBothEndsOfOneSession) {
    Leg one(24001, 24002), two(24003, 24004);
    ASSERT_TRUE(WaitReady(*one.client) && WaitReady(*one.server));
    ASSERT_TRUE(WaitReady(*two.client) && WaitReady(*two.server));
    const auto c = one.client->ExportBinding(), s = one.server->ExportBinding();
    ASSERT_TRUE(c.has_value());
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*c, *s) << "both ends of one session see one exporter value";
    const auto other = two.client->ExportBinding();
    ASSERT_TRUE(other.has_value());
    EXPECT_NE(*c, *other) << "two sessions never share one";
}

TEST(RelayIdentityBinding, MitmRelayIsDetectedOnBothEnds) {
    // A dials B through relay M. M terminates the QUIC session on both legs
    // (A <-> M, M <-> B) and copies the decrypted streams across byte for
    // byte, dineroid included.
    Node a, b;
    Leg a_m(24011, 24012), m_b(24013, 24014);
    std::atomic<bool> stop{false};
    std::thread bridge([&] {
        while (!stop.load()) {
            auto up = a_m.server->ReadDecryptedStream(std::chrono::milliseconds(20));
            if (!up.empty()) m_b.client->EnqueueOutgoingStream(std::move(up));
            auto down = m_b.client->ReadDecryptedStream(std::chrono::milliseconds(20));
            if (!down.empty()) a_m.server->EnqueueOutgoingStream(std::move(down));
        }
    });
    const auto key_a = a.mgr->test_install_quic_relay_peer(a_m.client, "8.8.10.1:20999", 0x51,
                                                           b.id->get_node_id_bytes());
    const auto key_b = b.mgr->test_install_quic_relay_peer(m_b.server, "8.8.10.1:20999", 0x52,
                                                           std::nullopt);
    ASSERT_FALSE(key_a.empty());
    ASSERT_FALSE(key_b.empty());

    const bool a_dropped = WaitDropped(*a.mgr, key_a);
    const bool b_dropped = WaitDropped(*b.mgr, key_b);
    stop = true;
    bridge.join();
    EXPECT_TRUE(a_dropped) << "A accepted the relay's forwarded identity";
    EXPECT_TRUE(b_dropped) << "B accepted the relay's forwarded identity";
    if (auto* p = a.mgr->get_peer_info(key_a)) EXPECT_FALSE(p->identity_proven);
    if (auto* p = b.mgr->get_peer_info(key_b)) EXPECT_FALSE(p->identity_proven);
}

TEST(RelayIdentityBinding, HonestRelayedSessionIsProven) {
    Node a, b;
    Leg link(24021, 24022);
    const auto key_a = a.mgr->test_install_quic_relay_peer(link.client, "8.8.10.1:20999", 0x61,
                                                           b.id->get_node_id_bytes());
    const auto key_b = b.mgr->test_install_quic_relay_peer(link.server, "8.8.10.1:20999", 0x62,
                                                           std::nullopt);
    ASSERT_TRUE(WaitProven(*a.mgr, key_a)) << "A never proved B over an honest relayed session";
    ASSERT_TRUE(WaitProven(*b.mgr, key_b));
    EXPECT_EQ(a.mgr->get_peer_info(key_a)->their_node_id, b.id->get_node_id_bytes());
}

TEST(RelayIdentityBinding, OutboundRelayPeerWithWrongIdentityIsDisconnected) {
    // A asks relay R for X; the far end is B (a valid identity, but not X).
    Node a, b;
    auto x = MakeIdentity();
    dinero::p2p::HintFields via_r;
    via_r.relay_addr = {8, 8, 10, 1};
    via_r.relay_port = 20999;
    via_r.expiry = static_cast<uint32_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 1800);
    P2PMessage hint;
    hint.command = "relayhn2";
    hint.payload = dinero::p2p::SignHint(*x, via_r);
    a.mgr->handle_relay_hint_v2("10.4.0.1:20999", hint);
    ASSERT_EQ(a.mgr->test_relay_hint_count(x->get_node_id_bytes()), 1u);

    Leg link(24031, 24032);
    const auto key_a = a.mgr->test_install_quic_relay_peer(link.client, "8.8.10.1:20999", 0x71,
                                                           x->get_node_id_bytes());
    b.mgr->test_install_quic_relay_peer(link.server, "8.8.10.1:20999", 0x72, std::nullopt);
    EXPECT_TRUE(WaitDropped(*a.mgr, key_a)) << "A kept a relayed peer that is not the node it dialed";
    if (auto* p = a.mgr->get_peer_info(key_a)) EXPECT_FALSE(p->identity_proven);
    EXPECT_EQ(a.mgr->test_relay_hint_count(x->get_node_id_bytes()), 0u)
        << "the hint that led to the wrong node must be dropped";
}

TEST(RelayIdentityBinding, RelayPeerWithoutV2ProofIsDisconnected) {
    Node a, b;
    b.mgr->test_set_dineroid_v1_only(true);  // an old build at the far end
    Leg link(24041, 24042);
    const auto key_a = a.mgr->test_install_quic_relay_peer(link.client, "8.8.10.1:20999", 0x81,
                                                           b.id->get_node_id_bytes());
    b.mgr->test_install_quic_relay_peer(link.server, "8.8.10.1:20999", 0x82, std::nullopt);
    EXPECT_TRUE(WaitDropped(*a.mgr, key_a)) << "a relayed peer without a session-bound proof";
    if (auto* p = a.mgr->get_peer_info(key_a)) EXPECT_FALSE(p->identity_proven);
}

TEST(RelayIdentityBinding, TcpPeerLegacyBehaviorUnchanged) {
    // Over TCP a v1 dineroid (an older build) still proves the identity.
    Node a(24051), b(24052);
    b.mgr->test_set_dineroid_v1_only(true);
    for (Node* n : {&a, &b}) {
        n->mgr->set_quic_enabled(false);
        ASSERT_TRUE(n->mgr->start());
    }
    ASSERT_TRUE(a.mgr->connect_to_peer("127.0.0.1", 24052));
    ASSERT_TRUE(WaitProven(*a.mgr, "127.0.0.1:24052")) << "TCP + dineroid v1 must keep working";
    EXPECT_EQ(a.mgr->get_peer_info("127.0.0.1:24052")->their_node_id, b.id->get_node_id_bytes());
}

TEST(RelayIdentityBinding, RelayPeerThatHidesDineroIdIsDisconnected) {
    // A relay that strips NODE_DINERO_V2 from the version must not get the
    // identity check skipped.
    Node a, b;
    b.mgr->set_service_flags_provider([] {
        return dinero::ServiceFlags::NODE_NETWORK | dinero::ServiceFlags::NODE_COMPACT_TIMING_V1;
    });
    Leg link(24061, 24062);
    const auto key_a = a.mgr->test_install_quic_relay_peer(link.client, "8.8.10.1:20999", 0x91,
                                                           b.id->get_node_id_bytes());
    b.mgr->test_install_quic_relay_peer(link.server, "8.8.10.1:20999", 0x92, std::nullopt);
    EXPECT_TRUE(WaitDropped(*a.mgr, key_a)) << "a QUIC peer that skipped dineroid was kept";
}
