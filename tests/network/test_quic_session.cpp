#include "network/quic_session.h"
#include "quic_loopback_pair.h"
#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
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

}  // namespace

TEST(QuicSession, LoopbackHandshakeAndOneEncryptedStreamPayload) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_EQ(info.crypto_backend, "ossl");
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;

    using dinero::network::QuicSession;

    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kTestCertificate;
    options.private_key_pem = kTestPrivateKey;
    options.verify_peer = false;

    dinero::network::test::QuicLoopbackPair pair;
    auto& client_session = pair.client;
    auto& server_session = pair.server;

    const auto client_addr = Localhost(22001);
    const auto server_addr = Localhost(22002);

    ASSERT_TRUE(server_session->StartServer(server_addr, client_addr, options));
    ASSERT_TRUE(client_session->StartClient(client_addr, server_addr, options));

    auto cr = client_session->WaitHandshakeReady();
    auto sr = server_session->WaitHandshakeReady();
    ASSERT_EQ(cr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(sr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(cr.get());
    ASSERT_TRUE(sr.get());

    const std::vector<uint8_t> payload = {0x64, 0x69, 0x6e, 0x65, 0x72, 0x6f,
                                          0x2d, 0x71, 0x75, 0x69, 0x63};
    client_session->EnqueueOutgoingStream(payload, true);

    std::vector<uint8_t> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < payload.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto chunk = server_session->ReadDecryptedStream(std::chrono::milliseconds(100));
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(received, payload);

    const auto client_stats = client_session->Stats();
    const auto server_stats = server_session->Stats();
    EXPECT_TRUE(client_stats.handshake_completed);
    EXPECT_TRUE(client_stats.handshake_confirmed);
    EXPECT_TRUE(server_stats.handshake_completed);
    EXPECT_TRUE(server_stats.handshake_confirmed);
    EXPECT_EQ(client_stats.selected_alpn, options.alpn);
    EXPECT_EQ(server_stats.selected_alpn, options.alpn);
    EXPECT_FALSE(client_stats.tls_cipher.empty());
    EXPECT_FALSE(server_stats.tls_cipher.empty());

    EXPECT_TRUE(info.mainnet_relay_ready);

    // Pair teardown disconnects both writers before joining either session.
}

TEST(QuicSession, PostHandshakeStreamSurvivesDelayedSecondSend) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_EQ(info.crypto_backend, "ossl");
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;

    using dinero::network::QuicSession;

    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kTestCertificate;
    options.private_key_pem = kTestPrivateKey;
    options.verify_peer = false;

    dinero::network::test::QuicLoopbackPair pair;
    auto& client_session = pair.client;
    auto& server_session = pair.server;

    const auto client_addr = Localhost(22101);
    const auto server_addr = Localhost(22102);

    ASSERT_TRUE(server_session->StartServer(server_addr, client_addr, options));
    ASSERT_TRUE(client_session->StartClient(client_addr, server_addr, options));

    auto cr = client_session->WaitHandshakeReady();
    auto sr = server_session->WaitHandshakeReady();
    ASSERT_EQ(cr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(sr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(cr.get());
    ASSERT_TRUE(sr.get());

    const std::vector<uint8_t> first = {'f', 'i', 'r', 's', 't'};
    const std::vector<uint8_t> second = {'s', 'e', 'c', 'o', 'n', 'd'};

    client_session->EnqueueOutgoingStream(first, false);
    std::vector<uint8_t> received;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < first.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto chunk = server_session->ReadDecryptedStream(std::chrono::milliseconds(100));
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    ASSERT_EQ(received, first);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    ASSERT_TRUE(client_session->active()) << client_session->last_error();
    ASSERT_TRUE(server_session->active()) << server_session->last_error();

    client_session->EnqueueOutgoingStream(second, false);
    received.clear();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < second.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto chunk = server_session->ReadDecryptedStream(std::chrono::milliseconds(100));
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(received, second);
    EXPECT_TRUE(client_session->active()) << client_session->last_error();
    EXPECT_TRUE(server_session->active()) << server_session->last_error();

    // Pair teardown disconnects both writers before joining either session.
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

namespace {

dinero::network::QuicSessionOptions TestOptions(const char* alpn) {
    dinero::network::QuicSessionOptions options;
    options.alpn = alpn;
    options.server_name = "localhost";
    options.certificate_pem = kTestCertificate;
    options.private_key_pem = kTestPrivateKey;
    options.verify_peer = false;
    return options;
}

}  // namespace

// Without a CONNECTION_CLOSE the peer only learns of a shutdown from the 60 s
// idle timeout, which is how a vanished peer and a polite one looked identical.
TEST(QuicSession, CloseNotifiesPeerPromptly) {
    const auto options = TestOptions("dinero-p2p/1");
    dinero::network::test::QuicLoopbackPair pair;
    const auto client_addr = Localhost(22201);
    const auto server_addr = Localhost(22202);
    ASSERT_TRUE(pair.server->StartServer(server_addr, client_addr, options));
    ASSERT_TRUE(pair.client->StartClient(client_addr, server_addr, options));
    auto cr = pair.client->WaitHandshakeReady();
    auto sr = pair.server->WaitHandshakeReady();
    ASSERT_EQ(cr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(sr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(cr.get());
    ASSERT_TRUE(sr.get());
    ASSERT_TRUE(pair.server->active());

    pair.client->Close();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pair.server->active() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_FALSE(pair.server->active())
        << "peer still active 2 s after Close(); last_error=" << pair.server->last_error();
}

// Direct peers ("dinero-p2p/1") and relay circuits ("dinero-relay/1") must not
// be able to complete a handshake with each other.
TEST(QuicSession, AlpnMismatchFailsHandshake) {
    dinero::network::test::QuicLoopbackPair pair;
    const auto client_addr = Localhost(22301);
    const auto server_addr = Localhost(22302);
    ASSERT_TRUE(pair.server->StartServer(server_addr, client_addr,
                                         TestOptions("dinero-relay/1")));
    ASSERT_TRUE(pair.client->StartClient(client_addr, server_addr,
                                         TestOptions("dinero-p2p/1")));
    auto cr = pair.client->WaitHandshakeReady();
    ASSERT_EQ(cr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(cr.get());
}

// A peer that silently disappears must end the session by the negotiated
// 60 s idle timeout, even while we keep trying to send to it.
TEST(QuicSession, SilentPeerEndsByIdleTimeout) {
    const auto options = TestOptions("dinero-p2p/1");
    dinero::network::test::QuicLoopbackPair pair;
    const auto client_addr = Localhost(22401);
    const auto server_addr = Localhost(22402);
    ASSERT_TRUE(pair.server->StartServer(server_addr, client_addr, options));
    ASSERT_TRUE(pair.client->StartClient(client_addr, server_addr, options));
    ASSERT_TRUE(pair.client->WaitHandshakeReady().get());
    ASSERT_TRUE(pair.server->WaitHandshakeReady().get());

    pair.SeverForTest();  // drop every packet in both directions from now on
    // Several payloads: the second and later wait behind the single send slot.
    for (int i = 0; i < 3; ++i) {
        pair.server->EnqueueOutgoingStream(std::vector<uint8_t>(100, static_cast<uint8_t>(i)));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(75);
    while (pair.server->active() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    EXPECT_FALSE(pair.server->active()) << "still active 75 s after the peer vanished; err="
                                        << pair.server->last_error();
}
