// The address manager must store, group and gossip routable IPv6 addresses:
// its validity check was dotted-quad only, so every IPv6 address any peer
// ever told us about was thrown away.

#include "p2p/addrman.h"

#include <gtest/gtest.h>

using dinero::p2p::AddrManNetGroup;
using dinero::p2p::NetworkAddress;

namespace {
NetworkAddress A(const std::string& ip, uint16_t port = 20999) {
    NetworkAddress a;
    a.ip = ip;
    a.port = port;
    return a;
}
}  // namespace

TEST(AddrManIpv6, AcceptsRoutableRejectsLocal) {
    EXPECT_TRUE(A("2600:1700:7de1:7170::2d").isValid());
    EXPECT_TRUE(A("2600:1700:7de1:7170::2d").isRoutable());
    for (const char* bad : {"::1", "fe80::1", "fd00::1", "fc00::5", "2001:db8::1", "::"}) {
        EXPECT_TRUE(A(bad).isValid()) << bad;
        EXPECT_FALSE(A(bad).isRoutable()) << bad;
    }
    EXPECT_FALSE(A("2600:zzzz::1").isValid());
    EXPECT_FALSE(A("1.2.3").isValid());
    EXPECT_TRUE(A("8.8.8.8").isRoutable());  // IPv4 unchanged
}

TEST(AddrManIpv6, MappedV4TreatedAsV4) {
    EXPECT_FALSE(A("::ffff:10.0.0.1").isRoutable());
    EXPECT_TRUE(A("::ffff:8.8.8.8").isRoutable());
    EXPECT_EQ(AddrManNetGroup("::ffff:8.8.4.4"), AddrManNetGroup("8.8.1.1"));
}

TEST(AddrManIpv6, NetGroupIs32ForV6And16ForV4) {
    EXPECT_EQ(AddrManNetGroup("2600:1700:a::1"), AddrManNetGroup("2600:1700:b::1"));
    EXPECT_NE(AddrManNetGroup("2600:1700::1"), AddrManNetGroup("2600:1701::1"));
    EXPECT_EQ(AddrManNetGroup("1.2.3.4"), AddrManNetGroup("1.2.200.9"));
    EXPECT_NE(AddrManNetGroup("1.2.3.4"), AddrManNetGroup("1.3.3.4"));
    EXPECT_NE(AddrManNetGroup("2600:1700::1"), AddrManNetGroup("1.2.3.4"));
}

TEST(AddrManIpv6, ToStringBracketsV6) {
    EXPECT_EQ(A("2600::1").toString(), "[2600::1]:20999");
    EXPECT_EQ(A("1.2.3.4").toString(), "1.2.3.4:20999");
}
