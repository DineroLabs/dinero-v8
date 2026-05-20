#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

dinero::network::UdpAddr Localhost(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return dinero::network::UdpAddr::FromIPv4(ip, port);
}

}  // namespace

TEST(QuicTransport, ReportsEncryptedDependencyButKeepsMainnetRelayGated) {
    const auto info = dinero::network::QuicTransport::CompileInfo();

    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;
    EXPECT_FALSE(info.ngtcp2_version.empty());
    EXPECT_FALSE(info.openssl_version.empty());
    EXPECT_NE(info.crypto_backend, "none");

    EXPECT_FALSE(info.mainnet_relay_ready);
    EXPECT_NE(info.disabled_reason.find("P2PManager"), std::string::npos);
    EXPECT_TRUE(dinero::network::QuicTransport::InitializeCrypto());
}

TEST(QuicTransport, StartsUdpTransportAndQueuesOneDatagram) {
    dinero::network::QuicTransport transport;
    dinero::network::QuicTransport::Options options;
    options.listen_port = 0;
    options.max_pending_datagrams = 4;

    ASSERT_TRUE(transport.Start(options)) << transport.last_error();
    ASSERT_TRUE(transport.active());
    ASSERT_NE(transport.bound_port(), 0);

    dinero::network::UdpSocket sender;
    ASSERT_TRUE(sender.Bind(0));

    const std::vector<uint8_t> payload = {0x64, 0x69, 0x6e, 0x65, 0x72, 0x6f};
    ASSERT_TRUE(sender.SendTo(Localhost(transport.bound_port()),
                              payload.data(),
                              payload.size()));

    dinero::network::QuicDatagram received;
    ASSERT_TRUE(transport.ReceiveDatagram(&received, std::chrono::seconds(2)));
    EXPECT_EQ(received.payload, payload);
    EXPECT_EQ(received.source.family, dinero::network::UdpAddr::Family::V4);
    EXPECT_EQ(received.source.ip[0], 127);

    sender.Stop();
    transport.Stop();
    EXPECT_FALSE(transport.active());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
