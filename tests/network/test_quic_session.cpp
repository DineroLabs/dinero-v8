#include "network/quic_session.h"
#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

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

dinero::network::UdpAddr Localhost(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return dinero::network::UdpAddr::FromIPv4(ip, port);
}

void DeliverClientPackets(dinero::network::QuicSession& client,
                          dinero::network::QuicSession& server,
                          const dinero::network::UdpAddr& client_addr,
                          const dinero::network::UdpAddr& server_addr,
                          const dinero::network::QuicSessionOptions& options) {
    std::vector<std::vector<uint8_t>> packets;
    ASSERT_TRUE(client.DrainOutgoing(&packets)) << client.last_error();
    for (const auto& packet : packets) {
        if (!server.active()) {
            ASSERT_TRUE(server.StartServerFromInitial(server_addr, client_addr, packet, options))
                << server.last_error();
        }
        ASSERT_TRUE(server.ReceivePacket(server_addr, client_addr, packet))
            << server.last_error();
    }
}

void DeliverServerPackets(dinero::network::QuicSession& client,
                          dinero::network::QuicSession& server,
                          const dinero::network::UdpAddr& client_addr,
                          const dinero::network::UdpAddr& server_addr) {
    if (!server.active()) {
        return;
    }
    std::vector<std::vector<uint8_t>> packets;
    ASSERT_TRUE(server.DrainOutgoing(&packets)) << server.last_error();
    for (const auto& packet : packets) {
        ASSERT_TRUE(client.ReceivePacket(client_addr, server_addr, packet))
            << client.last_error();
    }
}

}  // namespace

TEST(QuicSession, LoopbackHandshakeAndOneEncryptedStreamPayload) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_EQ(info.crypto_backend, "ossl");
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;

    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kTestCertificate;
    options.private_key_pem = kTestPrivateKey;
    options.verify_peer = false;

    const auto client_addr = Localhost(22001);
    const auto server_addr = Localhost(22002);

    dinero::network::QuicSession client;
    dinero::network::QuicSession server;
    ASSERT_TRUE(client.StartClient(client_addr, server_addr, options))
        << client.last_error();

    const std::vector<uint8_t> payload = {
        0x64, 0x69, 0x6e, 0x65, 0x72, 0x6f, 0x2d, 0x71, 0x75, 0x69, 0x63};

    bool queued_payload = false;
    std::vector<uint8_t> received;

    for (int i = 0; i < 1000 && received != payload; ++i) {
        DeliverClientPackets(client, server, client_addr, server_addr, options);
        DeliverServerPackets(client, server, client_addr, server_addr);

        if (server.active()) {
            ASSERT_TRUE(client.HandleExpiry()) << client.last_error();
            ASSERT_TRUE(server.HandleExpiry()) << server.last_error();
        }

        if (!queued_payload && client.handshake_ready() && server.handshake_ready()) {
            ASSERT_TRUE(client.QueueStreamData(payload, true)) << client.last_error();
            queued_payload = true;
        }

        auto chunk = server.TakeReceivedStreamData();
        received.insert(received.end(), chunk.begin(), chunk.end());

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(queued_payload);
    EXPECT_EQ(received, payload);

    const auto client_stats = client.Stats();
    const auto server_stats = server.Stats();
    EXPECT_TRUE(client_stats.handshake_completed);
    EXPECT_TRUE(client_stats.handshake_confirmed);
    EXPECT_TRUE(server_stats.handshake_completed);
    EXPECT_TRUE(server_stats.handshake_confirmed);
    EXPECT_EQ(client_stats.selected_alpn, options.alpn);
    EXPECT_EQ(server_stats.selected_alpn, options.alpn);
    EXPECT_FALSE(client_stats.tls_cipher.empty());
    EXPECT_FALSE(server_stats.tls_cipher.empty());

    EXPECT_TRUE(info.mainnet_relay_ready);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
