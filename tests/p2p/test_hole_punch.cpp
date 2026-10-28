// Relay-coordinated hole punching, pure logic: which remote addresses to try
// (IPv6 first, IPv4 only when both NATs are punchable and the peers are not
// behind the same public IP), who initiates, and how many / how often.

#include "p2p/hole_punch.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>

namespace {

using dinero::network::UdpAddr;
using dinero::p2p::ChoosePunchTargets;
using dinero::p2p::NatMappingType;
using dinero::p2p::PunchCandidates;
using dinero::p2p::PunchRole;
using dinero::p2p::PunchScheduler;
using dinero::p2p::RoleFor;

UdpAddr V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    const uint8_t ip[4] = {a, b, c, d};
    return UdpAddr::FromIPv4(ip, port);
}

UdpAddr V6(uint8_t last, uint16_t port) {
    uint8_t ip[16] = {0x20, 0x01, 0x0d, 0xb8};
    ip[15] = last;
    return UdpAddr::FromIPv6(ip, port);
}

bool Same(const UdpAddr& a, const UdpAddr& b) {
    return a.family == b.family && a.ip == b.ip && a.port == b.port;
}

PunchCandidates Cand(std::vector<UdpAddr> v6, std::optional<UdpAddr> v4, NatMappingType m) {
    PunchCandidates c;
    c.v6 = std::move(v6);
    c.v4 = v4;
    c.v4_mapping = m;
    return c;
}

std::array<uint8_t, 20> Id(uint8_t first) {
    std::array<uint8_t, 20> id{};
    id[0] = first;
    return id;
}

}  // namespace

TEST(HolePunch, Ipv6FirstThenIpv4) {
    const auto local = Cand({V6(1, 9000)}, V4(1, 1, 1, 1, 9000), NatMappingType::EndpointIndependent);
    const auto remote = Cand({V6(2, 9001), V6(3, 9001)}, V4(2, 2, 2, 2, 9001),
                             NatMappingType::EndpointIndependent);
    const auto t = ChoosePunchTargets(local, remote);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_TRUE(Same(t[0], remote.v6[0]));
    EXPECT_TRUE(Same(t[1], remote.v6[1]));
    EXPECT_TRUE(Same(t[2], *remote.v4));
}

TEST(HolePunch, SymmetricEitherSideSkipsIpv4) {
    auto local = Cand({V6(1, 9000)}, V4(1, 1, 1, 1, 9000), NatMappingType::EndpointDependent);
    auto remote = Cand({V6(2, 9001)}, V4(2, 2, 2, 2, 9001), NatMappingType::EndpointIndependent);
    auto t = ChoosePunchTargets(local, remote);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_TRUE(Same(t[0], remote.v6[0]));

    local.v4_mapping = NatMappingType::EndpointIndependent;
    remote.v4_mapping = NatMappingType::EndpointDependent;
    t = ChoosePunchTargets(local, remote);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_TRUE(Same(t[0], remote.v6[0]));
}

TEST(HolePunch, UnknownMappingIsAttempted) {
    const auto local = Cand({V6(1, 9000)}, V4(1, 1, 1, 1, 9000), NatMappingType::Unknown);
    const auto remote = Cand({V6(2, 9001)}, V4(2, 2, 2, 2, 9001), NatMappingType::Unknown);
    const auto t = ChoosePunchTargets(local, remote);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_TRUE(Same(t[1], *remote.v4));
}

TEST(HolePunch, NoLocalV6SkipsRemoteV6) {
    const auto local = Cand({}, V4(1, 1, 1, 1, 9000), NatMappingType::EndpointIndependent);
    const auto remote = Cand({V6(2, 9001)}, V4(2, 2, 2, 2, 9001),
                             NatMappingType::EndpointIndependent);
    const auto t = ChoosePunchTargets(local, remote);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_TRUE(Same(t[0], *remote.v4));
}

TEST(HolePunch, TargetsCappedAtThree) {
    std::vector<UdpAddr> v6;
    for (uint8_t i = 1; i <= 5; ++i) v6.push_back(V6(i, 9001));
    const auto local = Cand({V6(9, 9000)}, V4(1, 1, 1, 1, 9000), NatMappingType::EndpointIndependent);
    const auto remote = Cand(v6, V4(2, 2, 2, 2, 9001), NatMappingType::EndpointIndependent);
    const auto t = ChoosePunchTargets(local, remote);
    ASSERT_EQ(t.size(), 3u);
    for (size_t i = 0; i < 3; ++i) EXPECT_TRUE(Same(t[i], v6[i]));
}

TEST(HolePunch, SamePublicIpv4SkipsIpv4) {
    const auto local = Cand({}, V4(7, 7, 7, 7, 9000), NatMappingType::EndpointIndependent);
    const auto remote = Cand({}, V4(7, 7, 7, 7, 9555), NatMappingType::EndpointIndependent);
    EXPECT_TRUE(ChoosePunchTargets(local, remote).empty());
}

TEST(HolePunch, RoleWhenBothDialed) {
    const auto lo = Id(1), hi = Id(2);
    EXPECT_EQ(RoleFor(true, true, lo, hi), PunchRole::Initiator);
    EXPECT_EQ(RoleFor(true, true, hi, lo), PunchRole::Acceptor);
    EXPECT_EQ(RoleFor(true, false, hi, lo), PunchRole::Initiator);
    EXPECT_EQ(RoleFor(false, true, lo, hi), PunchRole::Acceptor);
    EXPECT_EQ(RoleFor(false, false, lo, hi), PunchRole::None);
}

TEST(HolePunch, SchedulerCapsInFlightAndBacksOffFailures) {
    using namespace std::chrono;
    auto now = steady_clock::time_point{} + hours(1);
    PunchScheduler s([&] { return now; });
    const auto a = Id(1), b = Id(2), c = Id(3), d = Id(4);

    EXPECT_TRUE(s.MayStart(a));
    s.Started(a);
    EXPECT_TRUE(s.MayStart(b));
    s.Started(b);
    EXPECT_FALSE(s.MayStart(c));

    s.Failed(a);  // frees a slot, starts a's backoff
    EXPECT_TRUE(s.MayStart(c));
    now += minutes(29);
    EXPECT_FALSE(s.MayStart(a));
    now += minutes(2);
    EXPECT_TRUE(s.MayStart(a));

    s.Succeeded(b);
    EXPECT_TRUE(s.MayStart(b));
    s.Started(d);
    s.Started(b);
    EXPECT_FALSE(s.MayStart(c));
}
