// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/quic_session.h"
#include "quic_loopback_pair.h"
#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Cert/key reused from test_quic_session.cpp — kept inline so the stress
// test can be moved/run independently.
constexpr auto kStressPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgwEvkGGgXAcRaG7Z8
gA7C6+W2RsW9gcjV9e5ybr0ikaahRANCAASCo35bDi+Q/q/CzHI1e5QaBrbqbFhW
G20QbVAeMK8l0oC8OGD3PSpZK1HXwALwzhMuwhxDos3ANb5naa5y17fQ
-----END PRIVATE KEY-----
)";

constexpr auto kStressCertificate = R"(-----BEGIN CERTIFICATE-----
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

dinero::network::QuicSessionOptions StressOptions() {
    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kStressCertificate;
    options.private_key_pem = kStressPrivateKey;
    options.verify_peer = false;
    return options;
}

// Run one full client/server handshake with the new threaded API; return
// true iff both sides reach handshake_ready within `timeout`.
bool RunOneHandshake(std::chrono::milliseconds timeout) {
    using dinero::network::QuicSession;

    const auto client_addr = Localhost(0);  // address identifiers only; no real socket is bound — delivery is via writer callbacks
    const auto server_addr = Localhost(0);

    dinero::network::test::QuicLoopbackPair pair;
    auto& client_session = pair.client;
    auto& server_session = pair.server;

    if (!server_session->StartServer(server_addr, client_addr, StressOptions())) {
        return false;
    }
    if (!client_session->StartClient(client_addr, server_addr, StressOptions())) {
        return false;
    }

    auto client_ready = client_session->WaitHandshakeReady();
    auto server_ready = server_session->WaitHandshakeReady();

    if (client_ready.wait_for(timeout) != std::future_status::ready) return false;
    if (server_ready.wait_for(timeout) != std::future_status::ready) return false;

    const bool result = client_ready.get() && server_ready.get();

    // Pair teardown disconnects both writers before joining either session.

    return result;
}

}  // namespace

TEST(QuicSessionStress, OneThousandLoopbackHandshakesAllSucceed) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_EQ(info.crypto_backend, "ossl");
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;

    constexpr int kIterations = 1000;
    constexpr auto kPerHandshakeTimeout = std::chrono::seconds(5);

    for (int i = 0; i < kIterations; ++i) {
        if (i % 100 == 0) std::cout << "handshake iteration " << i << std::endl;
        ASSERT_TRUE(RunOneHandshake(kPerHandshakeTimeout))
            << "handshake failed at iteration " << i;
    }
}

TEST(QuicSessionStress, IdleCloseWakesSessionThread) {
    for (int i = 0; i < 1000; ++i) {
        if (i % 100 == 0) std::cout << "idle close iteration " << i << std::endl;
        dinero::network::QuicSession session([](std::vector<uint8_t>) {});
        if (i % 2) std::this_thread::yield();
        // No connection/timer exists: shutdown is the only possible wakeup.
        session.Close();
    }
}

TEST(QuicSessionStress, TeardownDuringHandshakeIsSafe) {
    for (int i=0; i<100; ++i) {
        dinero::network::test::QuicLoopbackPair pair;
        ASSERT_TRUE(pair.server->StartServer(Localhost(0),Localhost(0),StressOptions()));
        ASSERT_TRUE(pair.client->StartClient(Localhost(0),Localhost(0),StressOptions()));
        // Exercise the timeout/assertion cleanup path without waiting for readiness.
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
