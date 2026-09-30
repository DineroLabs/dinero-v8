#include "p2p_manager.h"
#include "network/types.h"
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr uint64_t base = dinero::ServiceFlags::NODE_NETWORK;
constexpr uint64_t compact = dinero::ServiceFlags::NODE_COMPACT_TIMING_V1;
constexpr uint64_t orchard = dinero::ServiceFlags::NODE_ORCHARD_V1;
static_assert((orchard & (base | compact | dinero::ServiceFlags::NODE_WITNESS |
    dinero::ServiceFlags::NODE_NETWORK_LIMITED | dinero::ServiceFlags::NODE_UTREEXO |
    dinero::ServiceFlags::NODE_UTREEXO_BRIDGE | dinero::ServiceFlags::NODE_RELAY |
    dinero::ServiceFlags::NODE_DINERO_V2 | dinero::ServiceFlags::NODE_BEHIND_RELAY)) == 0);
struct OrchardSockets {
    int fd[2]{-1, -1};
    OrchardSockets() {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) throw std::runtime_error("socketpair");
    }
    ~OrchardSockets() { for (int f : fd) if (f >= 0) close(f); }
    void Send(const P2PMessage& message) {
        const auto wire = message.serialize();
        ASSERT_EQ(send(fd[1], wire.data(), wire.size(), 0), static_cast<ssize_t>(wire.size()));
    }
};
bool OrchardHandshake(P2PManager& manager, bool outbound, uint64_t flags) {
    OrchardSockets sockets;
    PeerInfo peer{};
    peer.address = "127.0.0.1";
    peer.port = 31001;
    peer.socket_fd = sockets.fd[0];
    peer.is_outbound = outbound;
    // Remote height, version and user agent never select the local policy.
    sockets.Send(P2PMessage::create_version(70016, UINT32_MAX, base | flags, "/orchard-v1/", 12346));
    sockets.Send(P2PMessage::create_verack());
    const bool result = manager.test_perform_handshake(&peer);
    EXPECT_EQ(peer.orchard_capable.load(), (flags & orchard) != 0);
    EXPECT_EQ(peer.compact_timing_capable.load(), (flags & compact) != 0);
    if (!result) {
        char wire[4096];
        const auto count = recv(sockets.fd[1], wire, sizeof(wire), MSG_DONTWAIT);
        EXPECT_GT(count, 0);
        if (count > 0) {
            const std::string response(wire, static_cast<size_t>(count));
            EXPECT_NE(response.find("upgrade-required:"), std::string::npos);
            EXPECT_NE(response.find("orchard-v1"), std::string::npos);
        }
    }
    return result;
}
TEST(OrchardPeerCutoff, BothHandshakeDirectionsRequireEachScheduledCapability) {
    for (bool outbound : {false, true}) {
        P2PManager manager(0);
        bool compact_active = false, orchard_active = false;
        manager.set_release_services_provider([&] {
            return (compact_active ? compact : 0) | (orchard_active ? orchard : 0);
        });
        EXPECT_TRUE(OrchardHandshake(manager, outbound, 0));
        orchard_active = true;
        EXPECT_FALSE(OrchardHandshake(manager, outbound, compact));
        EXPECT_TRUE(OrchardHandshake(manager, outbound, orchard));
        compact_active = true;
        EXPECT_FALSE(OrchardHandshake(manager, outbound, orchard));
        EXPECT_FALSE(OrchardHandshake(manager, outbound, compact));
        EXPECT_TRUE(OrchardHandshake(manager, outbound, compact | orchard));
        orchard_active = compact_active = false;
        EXPECT_TRUE(OrchardHandshake(manager, outbound, 0));
    }
}
TEST(OrchardPeerCutoff, EstablishedSessionsRespectOrchardForSweepReceiveAndSend) {
    for (bool supports : {false, true}) {
        for (bool sweep : {false, true}) {
            P2PManager manager(0);
            OrchardSockets sockets;
            bool active = false;
            manager.set_release_services_provider([&] {
                return compact | (active ? orchard : 0);
            });
            manager.test_install_connected_direct_peer("orchard-peer", sockets.fd[0], false, false, {});
            manager.test_set_release_capability("orchard-peer", true);
            manager.test_set_orchard_capability("orchard-peer", supports);
            int calls = 0;
            manager.set_message_handler([&](const std::string&, const P2PMessage&) { ++calls; });
            P2PMessage message;
            message.command = "tx";
            manager.test_process_message("orchard-peer", message);
            EXPECT_EQ(calls, 1);
            active = true;
            if (sweep) {
                manager.test_sweep_release_compatibility();
                EXPECT_EQ(manager.test_peer_socket_fd("orchard-peer") >= 0, supports);
            } else {
                manager.test_process_message("orchard-peer", message);
                EXPECT_EQ(calls, supports ? 2 : 1);
            }
            EXPECT_EQ(manager.send_to_peer("orchard-peer", P2PMessage::create_ping(1)), supports);
            manager.test_cleanup_peer("orchard-peer");
            sockets.fd[0] = -1;
        }
    }
}
TEST(OrchardPeerCutoff, IncompleteHandshakeAndLowLevelControlPolicy) {
    P2PManager manager(0);
    manager.set_release_services_provider([] { return compact | orchard; });
    OrchardSockets pending;
    manager.test_install_connected_direct_peer("pending", pending.fd[0], false, false, {});
    manager.test_set_release_capability("pending", false, false);
    manager.test_sweep_release_compatibility();
    EXPECT_GE(manager.test_peer_socket_fd("pending"), 0);
    manager.test_cleanup_peer("pending");
    pending.fd[0] = -1;
    OrchardSockets established;
    PeerInfo peer{};
    peer.socket_fd = established.fd[0];
    peer.release_handshake_complete.store(true);
    peer.compact_timing_capable.store(true);
    EXPECT_FALSE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
    peer.orchard_capable.store(true);
    EXPECT_TRUE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
    peer.compact_timing_capable.store(false);
    EXPECT_FALSE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
}
TEST(OrchardPeerCutoff, CapabilityMoveAndUnconfiguredVersionDoNotInventSupport) {
    PeerInfo peer{};
    peer.compact_timing_capable.store(true);
    peer.orchard_capable.store(true);
    peer.release_handshake_complete.store(true);
    PeerInfo moved(std::move(peer));
    EXPECT_TRUE(moved.compact_timing_capable.load());
    EXPECT_TRUE(moved.orchard_capable.load());
    EXPECT_TRUE(moved.release_handshake_complete.load());
    const auto message = P2PMessage::create_version(70016, 0, 0, "/fixture/", 12347);
    ASSERT_GE(message.payload.size(), 12u);
    uint64_t flags = 0;
    for (unsigned i = 0; i < 8; ++i) flags |= uint64_t(message.payload[4 + i]) << (8 * i);
    EXPECT_EQ(flags & orchard, 0u);
    EXPECT_NE(flags & compact, 0u);
}
TEST(OrchardPeerCutoff, CompletePolicyIsCapturedOnceForEachControlSend) {
    P2PManager manager(0);
    unsigned queries = 0;
    uint64_t required = compact | orchard;
    manager.set_release_services_provider([&] { ++queries; return required; });
    OrchardSockets sockets;
    PeerInfo peer{};
    peer.socket_fd = sockets.fd[0];
    peer.release_handshake_complete.store(true);
    peer.compact_timing_capable.store(true);
    EXPECT_FALSE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
    EXPECT_EQ(queries, 1u);
    peer.orchard_capable.store(true);
    EXPECT_TRUE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
    EXPECT_EQ(queries, 2u);
    required = 0; // validated boundary rewind, no permanent capability ban
    peer.compact_timing_capable.store(false);
    peer.orchard_capable.store(false);
    EXPECT_TRUE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
    EXPECT_EQ(queries, 3u);
}
} // namespace
