// Relay-coordinated hole punching, pure logic: which remote addresses to try
// (IPv6 first, IPv4 only when both NATs are punchable and the peers are not
// behind the same public IP), who initiates, and how many / how often.

#include "p2p/hole_punch.h"

#include "p2p_manager.h"
#include "network/types.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

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

// ---- Wire messages and the service bit -------------------------------------

TEST(HolePunchWire, RoundTripsV4AndV6) {
    PunchCandidates c;
    c.v4 = V4(203, 0, 113, 7, 40001);
    c.v4_mapping = NatMappingType::EndpointDependent;
    c.v6 = {V6(1, 20999), V6(2, 21000)};
    constexpr uint64_t nonce = 0x1122334455667788ULL;

    const std::pair<const char*, P2PMessage> msgs[] = {
        {"punchoff", P2PMessage::create_punch_offer(nonce, c)},
        {"punchans", P2PMessage::create_punch_answer(nonce, c)}};
    for (const auto& [cmd, msg] : msgs) {
        EXPECT_EQ(msg.command, cmd);
        const auto parsed = dinero::p2p::ParsePunchCandidates(msg.payload);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->first, nonce);
        ASSERT_TRUE(parsed->second.v4.has_value());
        EXPECT_EQ(parsed->second.v4->to_string(), "203.0.113.7:40001");
        EXPECT_EQ(parsed->second.v4_mapping, NatMappingType::EndpointDependent);
        ASSERT_EQ(parsed->second.v6.size(), 2u);
        EXPECT_EQ(parsed->second.v6[0].to_string(), c.v6[0].to_string());
        EXPECT_EQ(parsed->second.v6[1].to_string(), c.v6[1].to_string());
    }

    // v6 only, no v4.
    PunchCandidates only6;
    only6.v6 = {V6(9, 5)};
    const auto p6 = dinero::p2p::ParsePunchCandidates(
        P2PMessage::create_punch_offer(1, only6).payload);
    ASSERT_TRUE(p6.has_value());
    EXPECT_FALSE(p6->second.v4.has_value());
    EXPECT_EQ(p6->second.v4_mapping, NatMappingType::Unknown);

    const auto go = P2PMessage::create_punch_go(nonce);
    EXPECT_EQ(go.command, "punchgo");
    ASSERT_EQ(go.payload.size(), 8u);
    EXPECT_EQ(go.payload[0], 0x88);
    EXPECT_EQ(go.payload[7], 0x11);
}

TEST(HolePunchWire, RejectsMalformed) {
    PunchCandidates c;
    c.v4 = V4(203, 0, 113, 7, 40001);
    c.v6 = {V6(1, 20999)};
    const auto good = P2PMessage::create_punch_offer(7, c).payload;
    ASSERT_TRUE(dinero::p2p::ParsePunchCandidates(good).has_value());

    auto truncated = good;
    truncated.pop_back();
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates(truncated).has_value());

    auto trailing = good;
    trailing.push_back(0);
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates(trailing).has_value());

    // Header is nonce(8) | mapping(1) | count(1) | entries.
    auto count9 = good;
    count9[9] = 9;
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates(count9).has_value());

    auto unknown_net = good;
    unknown_net[10] = 3;  // first entry's net byte
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates(unknown_net).has_value());

    // Two v4 entries: nonce | mapping | count=2 | (1,a,b,c,d,port) x2.
    std::vector<uint8_t> two_v4(8, 0);
    two_v4.push_back(0);
    two_v4.push_back(2);
    for (int i = 0; i < 2; i++) {
        const uint8_t e[] = {1, 10, 0, 0, uint8_t(i), 0x1f, 0x90};
        two_v4.insert(two_v4.end(), e, e + sizeof(e));
    }
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates(two_v4).has_value());

    // Unknown mapping byte, and too-short header.
    auto bad_map = good;
    bad_map[8] = 3;
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates(bad_map).has_value());
    EXPECT_FALSE(dinero::p2p::ParsePunchCandidates({1, 2, 3}).has_value());
}

TEST(HolePunchWire, ServiceBit32SurvivesVersion) {
    using namespace dinero::ServiceFlags;
    static_assert(NODE_HOLEPUNCH == (1ULL << 32));
    int fd[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0);
    const uint64_t services = NODE_NETWORK | NODE_HOLEPUNCH | NODE_P2P_QUIC;
    const auto version = P2PMessage::create_version(70016, 0, services, "/t/", 99).serialize();
    const auto verack = P2PMessage::create_verack().serialize();
    ASSERT_EQ(send(fd[1], version.data(), version.size(), 0), static_cast<ssize_t>(version.size()));
    ASSERT_EQ(send(fd[1], verack.data(), verack.size(), 0), static_cast<ssize_t>(verack.size()));

    P2PManager manager(0);
    PeerInfo peer{};
    peer.address = "127.0.0.1";
    peer.port = 31000;
    peer.socket_fd = fd[0];
    peer.is_outbound = true;
    const bool ok = manager.test_perform_handshake(&peer);
    close(fd[0]);
    close(fd[1]);
    ASSERT_TRUE(ok);
    EXPECT_NE(peer.service_flags & NODE_HOLEPUNCH, 0u);
    EXPECT_NE(peer.service_flags & NODE_P2P_QUIC, 0u);
    EXPECT_EQ(peer.service_flags & ~services, 0u);
}
