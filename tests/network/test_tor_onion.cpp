#include "network/tor_control.h"
#include "p2p/addr_v2.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(TorControlReply, ParsesMultilineSuccess) {
    const std::string wire =
        "250-PROTOCOLINFO 1\r\n"
        "250-AUTH METHODS=COOKIE COOKIEFILE=\"/tmp/control_auth_cookie\"\r\n"
        "250 OK\r\n";
    dinero::network::TorControlReply reply;
    std::string error;
    ASSERT_TRUE(dinero::network::ParseTorControlReply(wire, &reply, &error))
        << error;
    EXPECT_EQ(reply.code, 250);
    ASSERT_EQ(reply.lines.size(), 3u);
    EXPECT_EQ(reply.lines[1],
              "AUTH METHODS=COOKIE COOKIEFILE=\"/tmp/control_auth_cookie\"");
}

TEST(TorControlReply, RejectsIncompleteOrMixedReply) {
    dinero::network::TorControlReply reply;
    std::string error;
    EXPECT_FALSE(dinero::network::ParseTorControlReply(
        "250-PROTOCOLINFO 1\r\n", &reply, &error));
    EXPECT_FALSE(dinero::network::ParseTorControlReply(
        "250-one\r\n551 two\r\n", &reply, &error));
}

TEST(TorV3Address, CanonicalRoundTripAndChecksumValidation) {
    std::vector<uint8_t> public_key(32);
    for (size_t i = 0; i < public_key.size(); ++i) {
        public_key[i] = static_cast<uint8_t>(i);
    }
    std::string onion;
    std::string error;
    ASSERT_TRUE(dinero::p2p::EncodeTorV3Address(public_key, &onion, &error))
        << error;
    EXPECT_EQ(onion.size(), 62u);
    EXPECT_EQ(onion.substr(56), ".onion");

    std::vector<uint8_t> decoded;
    ASSERT_TRUE(dinero::p2p::DecodeTorV3Address(onion, &decoded, &error))
        << error;
    EXPECT_EQ(decoded, public_key);

    onion[55] = onion[55] == 'a' ? 'b' : 'a';
    EXPECT_FALSE(dinero::p2p::DecodeTorV3Address(onion, &decoded, &error));
}

TEST(TorV3Address, Addrv2CarriesOnlyPublicKey) {
    std::vector<uint8_t> public_key(32, 0x5a);
    dinero::p2p::AddrV2Entry entry;
    entry.time = 42;
    entry.services = 1;
    entry.net = dinero::p2p::NetworkType::TORV3;
    entry.addr = public_key;
    entry.port = 20999;

    const auto wire = dinero::p2p::EncodeAddrV2({entry});
    std::vector<dinero::p2p::AddrV2Entry> decoded;
    std::string error;
    ASSERT_TRUE(dinero::p2p::DecodeAddrV2(wire, &decoded, &error)) << error;
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded.front().net, dinero::p2p::NetworkType::TORV3);
    EXPECT_EQ(decoded.front().addr, public_key);
    EXPECT_EQ(decoded.front().port, 20999);
}
