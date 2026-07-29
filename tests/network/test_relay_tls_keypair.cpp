// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for the ephemeral self-signed relay TLS keypair generator.
//
// Two layers of validation:
//   1. The generator returns non-empty PEM strings shaped like real PEM
//      blobs (header/footer markers, base64 body).
//   2. The cert+key actually drive a working ngtcp2/quictls handshake —
//      we wire a client and server QuicSession with the generated
//      material and confirm both reach handshake_ready and exchange one
//      stream payload. If ngtcp2 rejects the cert (e.g., missing required
//      extensions, bad signature), this test fails — making the
//      generator's correctness an empirical guarantee, not just a syntax
//      check.

#include "network/relay_tls_keypair.h"

#include "network/quic_session.h"
#include "network/quic_transport.h"
#include "network/udp_socket.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

dinero::network::UdpAddr Loopback(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return dinero::network::UdpAddr::FromIPv4(ip, port);
}

}  // namespace

TEST(RelayTlsKeypair, GeneratesPemShapedOutputs) {
    std::string cert_pem;
    std::string key_pem;
    std::string err;

    ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert_pem, &key_pem, &err))
        << err;
    ASSERT_TRUE(err.empty()) << err;

    // PEM markers — cheap structural check.
    EXPECT_NE(cert_pem.find("-----BEGIN CERTIFICATE-----"), std::string::npos);
    EXPECT_NE(cert_pem.find("-----END CERTIFICATE-----"), std::string::npos);
    EXPECT_NE(key_pem.find("-----BEGIN PRIVATE KEY-----"), std::string::npos);
    EXPECT_NE(key_pem.find("-----END PRIVATE KEY-----"), std::string::npos);

    // Cert and key bodies should be substantively sized (not empty PEMs).
    EXPECT_GT(cert_pem.size(), 256u);
    EXPECT_GT(key_pem.size(), 128u);
}

TEST(RelayTlsKeypair, RejectsNullOutputParameters) {
    std::string cert_pem;
    std::string key_pem;
    std::string err;
    EXPECT_FALSE(dinero::network::GenerateRelayTlsKeypair(nullptr, &key_pem, &err));
    EXPECT_FALSE(dinero::network::GenerateRelayTlsKeypair(&cert_pem, nullptr, &err));
}

TEST(RelayTlsKeypair, EachCallReturnsDistinctKeypair) {
    std::string cert_a, key_a, err_a;
    std::string cert_b, key_b, err_b;
    ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert_a, &key_a, &err_a))
        << err_a;
    ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert_b, &key_b, &err_b))
        << err_b;
    // Random serial + random key material → the two PEMs MUST differ.
    EXPECT_NE(cert_a, cert_b);
    EXPECT_NE(key_a, key_b);
}

TEST(RelayTlsKeypair, MaterialDrivesWorkingQuicHandshake) {
    // Skip cleanly on builds without crypto wired in. (Deliberately a SKIP,
    // not an ASSERT like test_quic_session.cpp: this test's subject is the
    // keypair GENERATOR, and a crypto-less build has nothing to say about it.)
    const auto info = dinero::network::QuicTransport::CompileInfo();
    if (!info.crypto_available || info.crypto_backend != "ossl") {
        GTEST_SKIP() << info.disabled_reason;
    }

    std::string cert_pem;
    std::string key_pem;
    std::string err;
    ASSERT_TRUE(dinero::network::GenerateRelayTlsKeypair(&cert_pem, &key_pem, &err))
        << err;

    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = cert_pem;
    options.private_key_pem = key_pem;
    options.verify_peer = false;

    // Writer-callback wiring, mirroring test_quic_session.cpp's
    // LoopbackHandshakeAndOneEncryptedStreamPayload — each session's
    // outbound packets are enqueued straight into the other's inbox.
    // (This file's previous DrainOutgoing/StartServerFromInitial pump
    // predates that API and is what bitrotted; see PR.)
    using dinero::network::QuicSession;
    std::shared_ptr<QuicSession> client_session;
    std::shared_ptr<QuicSession> server_session;
    auto client_writer = [&server_session](std::vector<uint8_t> bytes) {
        if (server_session) server_session->EnqueueIncomingPacket(std::move(bytes));
    };
    auto server_writer = [&client_session](std::vector<uint8_t> bytes) {
        if (client_session) client_session->EnqueueIncomingPacket(std::move(bytes));
    };
    server_session = std::make_shared<QuicSession>(server_writer);
    client_session = std::make_shared<QuicSession>(client_writer);

    const auto client_addr = Loopback(40001);
    const auto server_addr = Loopback(40002);

    ASSERT_TRUE(server_session->StartServer(server_addr, client_addr, options))
        << server_session->last_error();
    ASSERT_TRUE(client_session->StartClient(client_addr, server_addr, options))
        << client_session->last_error();

    auto cr = client_session->WaitHandshakeReady();
    auto sr = server_session->WaitHandshakeReady();
    ASSERT_EQ(cr.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "client handshake did not converge — generated cert/key are "
           "probably not acceptable to ngtcp2/quictls. client_err='"
        << client_session->last_error() << "'";
    ASSERT_EQ(sr.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "server handshake did not converge. server_err='"
        << server_session->last_error() << "'";
    EXPECT_TRUE(cr.get()) << client_session->last_error();
    EXPECT_TRUE(sr.get()) << server_session->last_error();

    // The header comment promises handshake AND one exchanged payload —
    // restore the second half of that guarantee (the bitrotted version had
    // quietly dropped it).
    const std::vector<uint8_t> payload = {'r', 'e', 'l', 'a', 'y', '-',
                                          't', 'l', 's', '-', 'o', 'k'};
    client_session->EnqueueOutgoingStream(payload, /*fin=*/true);

    std::vector<uint8_t> received;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < payload.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto chunk =
            server_session->ReadDecryptedStream(std::chrono::milliseconds(100));
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(received, payload)
        << "encrypted stream payload did not round-trip over the handshake "
           "driven by the generated keypair";

    // Explicit teardown so session destructors run while both sessions are
    // still in scope, making the by-reference writer captures safe.
    client_session->Close();
    server_session->Close();
    client_session.reset();
    server_session.reset();
}
