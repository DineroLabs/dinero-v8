#include "p2p_manager.h"
#include "network/types.h"
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <thread>

namespace {
struct SocketPair {
    int fd[2]{-1,-1};
    SocketPair() { if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) throw std::runtime_error("socketpair"); }
    ~SocketPair() { for (int f : fd) if (f >= 0) close(f); }
    void Send(const P2PMessage& message) {
        const auto wire = message.serialize();
        ASSERT_EQ(send(fd[1], wire.data(), wire.size(), 0), static_cast<ssize_t>(wire.size()));
    }
};

bool Handshake(P2PManager& manager, bool outbound, uint64_t services, uint32_t remote_height = 999999) {
    SocketPair sockets;
    PeerInfo peer{};
    peer.address = "127.0.0.1"; peer.port = 31000;
    peer.socket_fd = sockets.fd[0]; peer.is_outbound = outbound;
    // Same 70016 as v8.1.12, and a spoofed modern UA: neither proves support.
    sockets.Send(P2PMessage::create_version(70016, remote_height, services, "/v8.1.13/", 12345));
    sockets.Send(P2PMessage::create_verack());
    const bool result = manager.test_perform_handshake(&peer);
    if (!result) {
        char wire[4096];
        const auto bytes = recv(sockets.fd[1], wire, sizeof(wire), MSG_DONTWAIT);
        EXPECT_GT(bytes, 0);
        if (bytes > 0) {
            const std::string response(wire, static_cast<size_t>(bytes));
            EXPECT_NE(response.find("upgrade-required: compact-v1-60s-v1"), std::string::npos);
        }
    }
    return result;
}
constexpr uint64_t legacy = dinero::ServiceFlags::NODE_NETWORK;
constexpr uint64_t upgraded = legacy | dinero::ServiceFlags::NODE_COMPACT_TIMING_V1;

TEST(ReleasePeerCutoff, BothHandshakeDirectionsUseCurrentLocalPolicy) {
    for (bool outbound : {false, true}) {
        P2PManager manager(0);
        bool active = false;
        manager.set_release_cutoff_provider([&] { return active; });
        manager.set_height_provider([] { return 10u; });
        EXPECT_TRUE(Handshake(manager, outbound, legacy));
        active = true;
        EXPECT_FALSE(Handshake(manager, outbound, legacy));
        EXPECT_TRUE(Handshake(manager, outbound, upgraded, 0));
        active = false; // reorg: no permanent version ban
        EXPECT_TRUE(Handshake(manager, outbound, legacy));
    }
}

TEST(ReleasePeerCutoff, IdleSweepClosesOnlyUnsupportedEstablishedSessions) {
    P2PManager manager(0);
    bool active = false;
    manager.set_release_cutoff_provider([&] { return active; });
    for (bool capable : {false, true}) {
        SocketPair sockets;
        const auto key = capable ? "new" : "old";
        manager.test_install_connected_direct_peer(key, sockets.fd[0], false, false, {});
        manager.test_set_release_capability(key, capable);
        active = false;
        manager.test_sweep_release_compatibility();
        EXPECT_GE(manager.test_peer_socket_fd(key), 0);
        active = true;
        manager.test_sweep_release_compatibility();
        EXPECT_EQ(manager.test_peer_socket_fd(key) >= 0, capable);
        manager.test_cleanup_peer(key);
        sockets.fd[0] = -1;
    }
}

TEST(ReleasePeerCutoff, ExistingConnectionCannotDispatchOrSendAfterCutoff) {
    for (bool capable : {false, true}) {
        P2PManager manager(0);
        SocketPair sockets;
        bool active = false;
        manager.set_release_cutoff_provider([&] { return active; });
        manager.test_install_connected_direct_peer("peer", sockets.fd[0], false, false, {});
        manager.test_set_release_capability("peer", capable);
        int calls = 0;
        manager.set_message_handler([&](const std::string&, const P2PMessage&) { ++calls; });
        P2PMessage message; message.command = "tx";
        manager.test_process_message("peer", message);
        EXPECT_EQ(calls, 1);
        active = true;
        manager.test_process_message("peer", message);
        EXPECT_EQ(calls, capable ? 2 : 1);
        EXPECT_EQ(manager.send_to_peer("peer", P2PMessage::create_ping(1)), capable);
        manager.test_cleanup_peer("peer");
        sockets.fd[0] = -1;
    }
}
TEST(ReleasePeerCutoff, CutoffCrossingWhileHandshakeWaitsIsRechecked) {
    for (bool outbound : {false, true}) {
        P2PManager manager(0);
        int queries = 0;
        manager.set_release_cutoff_provider([&] { return ++queries >= 2; });
        EXPECT_FALSE(Handshake(manager, outbound, legacy));
        EXPECT_GE(queries, 2);
    }
}

TEST(ReleasePeerCutoff, IncompleteHandshakeIsNotAnIdleLegacySession) {
    P2PManager manager(0);
    manager.set_release_cutoff_provider([] { return true; });
    SocketPair sockets;
    manager.test_install_connected_direct_peer("pending", sockets.fd[0], false, false, {});
    manager.test_set_release_capability("pending", false, false);
    manager.test_sweep_release_compatibility();
    EXPECT_GE(manager.test_peer_socket_fd("pending"), 0);
    manager.test_cleanup_peer("pending"); sockets.fd[0] = -1;
}

TEST(ReleasePeerCutoff, BroadcastQueuedBeforeCutoffCannotEscapeAfterIt) {
    for (bool capable : {false, true}) {
        P2PManager manager(0);
        SocketPair sockets;
        bool active = false;
        manager.set_release_cutoff_provider([&] { return active; });
        manager.test_install_connected_direct_peer("peer", sockets.fd[0], false, false, {});
        manager.test_set_release_capability("peer", capable);
        manager.broadcast_message_async(P2PMessage::create_ping(123));
        active = true; // no outbox thread has consumed the queued frame yet
        std::thread worker([&] { manager.test_run_outbox(); });
        pollfd ready{sockets.fd[1], POLLIN, 0};
        const auto polled = poll(&ready, 1, 1000);
        char wire[128];
        const auto received = polled > 0 ? recv(sockets.fd[1], wire, sizeof(wire), 0) : -1;
        manager.test_stop_outbox(); worker.join();
        EXPECT_GT(polled, 0);
        EXPECT_EQ(received > 0, capable);
        if (!capable) EXPECT_EQ(received, 0); // actual socket closed, no frame leaked
        manager.test_cleanup_peer("peer"); sockets.fd[0] = -1;
    }
}

TEST(ReleasePeerCutoff, LowLevelControlSendsRespectEstablishedPeerPolicy) {
    P2PManager manager(0);
    manager.set_release_cutoff_provider([] { return true; });
    SocketPair sockets;
    PeerInfo peer{};
    peer.socket_fd = sockets.fd[0];
    peer.release_handshake_complete.store(true);
    EXPECT_FALSE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
    peer.compact_timing_capable.store(true);
    EXPECT_TRUE(manager.test_send_peer_message(&peer, P2PMessage::create_ping(1)));
}

} // namespace
