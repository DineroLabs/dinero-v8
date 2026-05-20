#include "p2p_manager.h"
#include "network/quic_session.h"
#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kCircuitId = 0x0102030405060708ULL;
const char* kRelayPeer = "127.0.0.1:20999";
const char* kVirtualPeer = "relay:test-target:0102030405060708";

constexpr auto kTestPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgwEvkGGgXAcRaG7Z8
gA7C6+W2RsW9gcjV9e5ybr0ikaahRANCAASCo35bDi+Q/q/CzHI1e5QaBrbqbFhW
G20QbVAeMK8l0oC8OGD3PSpZK1HXwALwzhMuwhxDos3ANb5naa5y17fQ
-----END PRIVATE KEY-----
)";

constexpr auto kTestCertificate = R"(-----BEGIN CERTIFICATE-----
MIICBzCCAa2gAwIBAgIUd2l6Pce3S0QH3dQC0Q/CjHbmggowCgYIKoZIzj0EAwIw
WTELMAkGA1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGElu
dGVybmV0IFdpZGdpdHMgUHR5IEx0ZDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI1
MTExNDExNTcwMFoXDTI1MTIxNDExNTcwMFowWTELMAkGA1UEBhMCQVUxEzARBgNV
BAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGEludGVybmV0IFdpZGdpdHMgUHR5IEx0
ZDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE
gqN+Ww4vkP6vwsxyNXuUGga26mxYVhttEG1QHjCvJdKAvDhg9z0qWStR18AC8M4T
LsIcQ6LNwDW+Z2mucte30KNTMFEwHQYDVR0OBBYEFFVgXLoLwzpf6+twP5z8Ujr2
5mxnMB8GA1UdIwQYMBaAFFVgXLoLwzpf6+twP5z8Ujr25mxnMA8GA1UdEwEB/wQF
MAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhAO4tnDNRAcooz62vf2m7vTyDqFCjcaIv
SJ9Gq0lvEXEcAiBwWBNUASBqLaje3hmtgwxcF7EIqqiGo5j8f9Ufgu6SRg==
-----END CERTIFICATE-----
)";

P2PMessage MakePing(uint64_t nonce) {
    return P2PMessage::create_ping(nonce);
}

std::string InstallVirtualPeer(P2PManager& manager) {
    return manager.test_install_virtual_relay_peer(
        kVirtualPeer,
        kRelayPeer,
        kCircuitId,
        P2PMessage::RelayDirection::ClientToTarget,
        true);
}

std::string InstallTargetVirtualPeer(P2PManager& manager) {
    return manager.test_install_virtual_relay_peer(
        kVirtualPeer,
        kRelayPeer,
        kCircuitId,
        P2PMessage::RelayDirection::TargetToClient,
        false);
}

dinero::network::UdpAddr Localhost(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return dinero::network::UdpAddr::FromIPv4(ip, port);
}

dinero::network::UdpAddr RelayQuicTestAddress(bool client_side) {
    const auto slot = static_cast<uint16_t>(kCircuitId & 0x0fff);
    const auto base = static_cast<uint16_t>(22000 + slot * 2);
    return Localhost(static_cast<uint16_t>(base + (client_side ? 1 : 2)));
}

std::vector<uint8_t> FrameForQuicStream(const P2PMessage& msg) {
    const auto payload = msg.serialize();
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 9);
    const uint64_t len = payload.size();
    if (len < 253) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFFu) {
        out.push_back(253);
        out.push_back(static_cast<uint8_t>(len & 0xff));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    } else if (len <= 0xFFFFFFFFu) {
        out.push_back(254);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xff));
        }
    } else {
        out.push_back(255);
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xff));
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

TEST(RelayVirtualTransport, MainnetPlaintextRelayDataIsRefusedByDefault) {
    P2PManager manager(0);
    const auto peer_key = InstallVirtualPeer(manager);

    EXPECT_FALSE(manager.test_plaintext_relay_transport_allowed());

    const auto ping = MakePing(0xabc123);
    const auto relay_frame = P2PMessage::create_relay_data(
        kCircuitId,
        P2PMessage::RelayDirection::TargetToClient,
        ping.serialize());

    manager.handle_relay_data(kRelayPeer, relay_frame);

    auto received = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(5));
    EXPECT_EQ(received, nullptr);
}

TEST(RelayVirtualTransport, DevOverrideAllowsRelayDataToQueueOneInnerMessage) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    const auto peer_key = InstallVirtualPeer(manager);

    ASSERT_TRUE(manager.test_plaintext_relay_transport_allowed());

    const auto ping = MakePing(0x424242);
    const auto relay_frame = P2PMessage::create_relay_data(
        kCircuitId,
        P2PMessage::RelayDirection::TargetToClient,
        ping.serialize());

    manager.handle_relay_data(kRelayPeer, relay_frame);

    auto received = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(50));
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->command, "ping");
    EXPECT_EQ(received->payload, ping.payload);

    auto empty = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(5));
    EXPECT_EQ(empty, nullptr);
}

TEST(RelayVirtualTransport, SocketlessVirtualPeerReceiveUsesTransportQueue) {
    P2PManager manager(0);
    manager.set_plaintext_relay_dev_override_for_tests(true);
    const auto peer_key = InstallVirtualPeer(manager);

    const auto ping = MakePing(0x515151);
    ASSERT_TRUE(manager.test_enqueue_relay_frame(peer_key, ping.serialize()));

    auto received = manager.test_receive_peer_message(
        peer_key, std::chrono::milliseconds(50));
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->command, "ping");
    EXPECT_EQ(received->payload, ping.payload);
}

TEST(RelayVirtualTransport, EncryptedQuicRelayDataQueuesDecryptedP2PFrame) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    if (!info.crypto_available || info.crypto_backend != "ossl") {
        GTEST_SKIP() << info.disabled_reason;
    }

    P2PManager manager(0);
    manager.set_encrypted_relay_dev_override_for_tests(true);
    const auto peer_key = InstallTargetVirtualPeer(manager);

    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kTestCertificate;
    options.private_key_pem = kTestPrivateKey;
    options.verify_peer = false;
    ASSERT_TRUE(manager.test_configure_relay_quic_server(peer_key, options));

    const auto client_addr = RelayQuicTestAddress(true);
    const auto server_addr = RelayQuicTestAddress(false);
    dinero::network::QuicSession client;
    ASSERT_TRUE(client.StartClient(client_addr, server_addr, options))
        << client.last_error();

    const auto ping = MakePing(0x717171);
    bool queued_payload = false;
    std::unique_ptr<P2PMessage> received;

    for (int i = 0; i < 1000 && !received; ++i) {
        std::vector<std::vector<uint8_t>> client_packets;
        ASSERT_TRUE(client.DrainOutgoing(&client_packets)) << client.last_error();
        for (const auto& packet : client_packets) {
            auto relay_frame = P2PMessage::create_relay_data(
                kCircuitId,
                P2PMessage::RelayDirection::ClientToTarget,
                packet);
            manager.handle_relay_data(kRelayPeer, relay_frame);
        }

        std::vector<std::vector<uint8_t>> server_packets;
        ASSERT_TRUE(manager.test_drain_relay_quic_packets(peer_key, &server_packets));
        for (const auto& packet : server_packets) {
            ASSERT_TRUE(client.ReceivePacket(client_addr, server_addr, packet))
                << client.last_error();
        }
        ASSERT_TRUE(client.HandleExpiry()) << client.last_error();

        if (!queued_payload && client.handshake_ready() &&
            manager.test_relay_quic_handshake_ready(peer_key)) {
            ASSERT_TRUE(client.QueueStreamData(FrameForQuicStream(ping), false))
                << client.last_error();
            queued_payload = true;
        }

        received = manager.test_receive_peer_message(
            peer_key, std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(queued_payload);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->command, "ping");
    EXPECT_EQ(received->payload, ping.payload);
    EXPECT_FALSE(info.mainnet_relay_ready);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
