// IPv6-safe peer keys. "ip:port" is ambiguous for IPv6 (2600::1:20999), so
// every key is formatted "[v6]:port" and every parser splits on the bracket,
// never on the last colon.

#include "network/endpoint_format.h"
#include "p2p_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using dinero::network::FormatEndpoint;
using dinero::network::SplitEndpoint;

namespace {
struct Split { bool ok; std::string host; uint16_t port; bool has_port; };
Split Do(const std::string& s) {
    Split r{};
    r.ok = SplitEndpoint(s, &r.host, &r.port, &r.has_port);
    return r;
}
}  // namespace

TEST(PeerKey, Ipv6RoundTripsThroughEveryParser) {
    for (const auto& [host, port] : std::vector<std::pair<std::string, uint16_t>>{
             {"2600:1700:7de1:7170::2d", 20999}, {"1.2.3.4", 20999}, {"::1", 1}}) {
        const std::string key = FormatEndpoint(host, port);
        const auto r = Do(key);
        EXPECT_TRUE(r.ok) << key;
        EXPECT_TRUE(r.has_port) << key;
        EXPECT_EQ(r.host, host) << key;
        EXPECT_EQ(r.port, port) << key;
    }
    EXPECT_EQ(FormatEndpoint("2600:1700::2d", 20999), "[2600:1700::2d]:20999");
    EXPECT_EQ(FormatEndpoint("1.2.3.4", 20999), "1.2.3.4:20999");
    // A bare IPv6 without brackets has no port, and must not be split on a colon.
    const auto bare = Do("2600:1700::2d");
    EXPECT_TRUE(bare.ok);
    EXPECT_FALSE(bare.has_port);
    EXPECT_EQ(bare.host, "2600:1700::2d");
    EXPECT_FALSE(Do("[2600::1]x").ok);
    EXPECT_FALSE(Do("[]:1").ok);
}

TEST(PeerKey, PeerInfoToStringBracketsIpv6) {
    PeerInfo p;
    p.address = "2600:1700:7de1:7170::2d";
    p.port = 20999;
    EXPECT_EQ(p.to_string(), "[2600:1700:7de1:7170::2d]:20999");
    p.address = "1.2.3.4";
    EXPECT_EQ(p.to_string(), "1.2.3.4:20999");
}

TEST(PeerKey, BanMatchesIpv6ExactAndCidr) {
    P2PManager mgr(0);
    ASSERT_TRUE(mgr.ban_peer("2600:1700::/32", std::chrono::seconds(600)));
    EXPECT_TRUE(mgr.is_peer_banned("2600:1700::5", 1));
    EXPECT_FALSE(mgr.is_peer_banned("2601::5", 1));
    ASSERT_TRUE(mgr.ban_peer("[2a00::9]:20999", std::chrono::seconds(600)));
    EXPECT_TRUE(mgr.is_peer_banned("2a00::9", 20999));
    EXPECT_FALSE(mgr.is_peer_banned("2a00::9", 20998));
}

#include "p2p/addr_v2.h"
#include "p2p/addrman.h"
#include "network/types.h"

TEST(AddrV2, Ipv6EntriesAreStoredNotDropped) {
    P2PManager mgr(20999);
    dinero::p2p::AddressManager addrman;
    mgr.set_address_manager(&addrman);
    dinero::p2p::AddrV2Entry e;
    e.time = static_cast<uint32_t>(std::time(nullptr));
    e.services = dinero::ServiceFlags::NODE_NETWORK;
    e.net = dinero::p2p::NetworkType::IPV6;
    e.addr = {0x26, 0x00, 0x17, 0x00, 0x7d, 0xe1, 0x71, 0x70, 0, 0, 0, 0, 0, 0, 0, 0x2d};
    e.port = 20999;
    P2PMessage msg;
    msg.command = dinero::MessageCommands::ADDRV2;
    msg.payload = dinero::p2p::EncodeAddrV2({e});
    mgr.test_process_message("203.0.113.1:20999", msg);
    bool found = false;
    for (const auto& a : addrman.getAddresses(1000)) {
        if (a.ip == "2600:1700:7de1:7170::2d" && a.port == 20999) found = true;
    }
    EXPECT_TRUE(found) << "gossiped IPv6 address was dropped";
}
