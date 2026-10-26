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

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

namespace {
// A free TCP port on ::1, or 0 if this host cannot bind IPv6 loopback.
uint16_t FreeV6Port() {
    int t = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (t < 0) return 0;
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    if (::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(t); return 0; }
    socklen_t len = sizeof(a);
    ::getsockname(t, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(t);
    return ntohs(a.sin6_port);
}

const PeerInfo* WaitConnected(P2PManager& m, const std::string& key, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto* p = m.get_peer_info(key); p && p->is_connected) return p;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}
}  // namespace

TEST(PeerKey, Ipv6LoopbackDialHandshake) {
    // Dual-stack: a v6 listener next to the v4 one, v6 dials, and the version
    // message carrying the v6 address the peer saw us at.
    const uint16_t port_a = FreeV6Port(), port_b = FreeV6Port();
    if (port_a == 0 || port_b == 0) {
        std::cout << "[  SKIPPED ] ::1 cannot be bound on this host" << std::endl;
        GTEST_SKIP() << "::1 cannot be bound on this host";
    }
    P2PManager a(port_a), b(port_b);
    for (P2PManager* m : {&a, &b}) {
        m->set_quic_enabled(false);
        m->set_height_provider([] { return 10u; });
        ASSERT_TRUE(m->start());
    }
    ASSERT_TRUE(b.connect_to_peer("::1", port_a));
    const PeerInfo* out = WaitConnected(b, "[::1]:" + std::to_string(port_a));
    ASSERT_NE(out, nullptr) << "outbound v6 peer never connected";
    // A put the address it saw B at into addrRecv, in v6 form.
    EXPECT_EQ(out->reported_local_addr, "::1");

    bool inbound_v6 = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!inbound_v6 && std::chrono::steady_clock::now() < deadline) {
        for (const auto& p : a.get_connected_peers()) {
            if (!p.is_outbound && p.is_connected && p.address == "::1") inbound_v6 = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(inbound_v6) << "A never accepted B over its v6 listener";
    b.stop();
    a.stop();
}

TEST(PeerKey, Ipv6InboundLimitIsPerSlash64) {
    // Every home and VPS gets a whole /64: a per-address limit would let one
    // host fill every inbound slot from 12 * 2^64 addresses.
    P2PManager m(0);
    m.test_set_inbound_limits(/*total=*/100, /*per_ip=*/12);
    for (int i = 1; i <= 12; ++i) {
        m.test_install_connected_direct_peer("2600:1700:1:2::" + std::to_string(i), -1,
                                             /*is_outbound=*/false, false, {});
    }
    EXPECT_FALSE(m.test_inbound_slot_available("2600:1700:1:2::ff")) << "13th peer from one /64 admitted";
    EXPECT_FALSE(m.test_inbound_slot_available("2600:1700:1:2:abcd::1"));
    EXPECT_TRUE(m.test_inbound_slot_available("2600:1700:1:3::1")) << "a neighbouring /64 is someone else";
    EXPECT_TRUE(m.test_inbound_slot_available("1.2.3.4"));
}
