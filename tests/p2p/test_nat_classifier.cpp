// NAT mapping behaviour (RFC 4787 §4.1) from one round of STUN answers taken
// on the P2P UDP socket: does the router keep one public mapping for our port
// whatever the destination (endpoint-independent, punchable), and does it keep
// our port number?

#include "p2p/nat_classifier.h"

#include <gtest/gtest.h>

namespace {

using dinero::network::UdpAddr;
using dinero::p2p::Classify;
using dinero::p2p::NatMappingType;

UdpAddr V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    const uint8_t ip[4] = {a, b, c, d};
    return UdpAddr::FromIPv4(ip, port);
}

const UdpAddr kServerA = V4(173, 249, 200, 59, 3478);
const UdpAddr kServerB = V4(172, 93, 167, 32, 3478);
const UdpAddr kServerC = V4(74, 125, 250, 129, 19302);

}  // namespace

TEST(NatClassifier, SameReflexiveAcrossServersIsIndependent) {
    // The owner's AT&T router, measured: local 45123 -> 162.200.227.214:45123
    // from three servers.
    const UdpAddr refl = V4(162, 200, 227, 214, 45123);
    auto m = Classify(45123, {{kServerA, refl}, {kServerB, refl}, {kServerC, refl}});
    EXPECT_EQ(m.mapping, NatMappingType::EndpointIndependent);
    EXPECT_TRUE(m.port_preserving);
}

TEST(NatClassifier, DifferentPortsIsDependent) {
    auto m = Classify(45123, {{kServerA, V4(162, 200, 227, 214, 50001)},
                              {kServerB, V4(162, 200, 227, 214, 50002)}});
    EXPECT_EQ(m.mapping, NatMappingType::EndpointDependent);
    EXPECT_FALSE(m.port_preserving);
}

TEST(NatClassifier, OneServerIsUnknown) {
    const UdpAddr refl = V4(162, 200, 227, 214, 45123);
    EXPECT_EQ(Classify(45123, {{kServerA, refl}}).mapping, NatMappingType::Unknown);
    // Two answers from the same server IP are still one vantage point.
    const UdpAddr a2 = V4(173, 249, 200, 59, 3479);
    EXPECT_EQ(Classify(45123, {{kServerA, refl}, {a2, refl}}).mapping, NatMappingType::Unknown);
    EXPECT_EQ(Classify(45123, {}).mapping, NatMappingType::Unknown);
}

TEST(NatClassifier, PortPreservingDetected) {
    const UdpAddr kept = V4(162, 200, 227, 214, 45123);
    const UdpAddr moved = V4(162, 200, 227, 214, 61000);
    EXPECT_TRUE(Classify(45123, {{kServerA, kept}, {kServerB, kept}}).port_preserving);
    auto m = Classify(45123, {{kServerA, moved}, {kServerB, moved}});
    EXPECT_EQ(m.mapping, NatMappingType::EndpointIndependent);
    EXPECT_FALSE(m.port_preserving);
}

TEST(NatClassifier, Ipv6AnswersDoNotCountAsAMappingChange) {
    // A v6 server sees our v6 address; that is a different socket family, not a
    // second NAT mapping, so it must not turn a v4 classification "dependent".
    const UdpAddr refl = V4(162, 200, 227, 214, 45123);
    const uint8_t s6[16] = {0x20, 0x01, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88};
    const uint8_t r6[16] = {0x26, 0x00, 0x17, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    auto m = Classify(45123, {{kServerA, refl}, {kServerB, refl},
                              {UdpAddr::FromIPv6(s6, 3478), UdpAddr::FromIPv6(r6, 45123)}});
    EXPECT_EQ(m.mapping, NatMappingType::EndpointIndependent);
}

TEST(NatClassifier, WireNamesMatchContract) {
    // getnetworkinfo.nat.mapping values, read by Plan 2c's reports and Plan 4.
    EXPECT_STREQ(dinero::p2p::NatMappingName(NatMappingType::EndpointIndependent), "endpoint_independent");
    EXPECT_STREQ(dinero::p2p::NatMappingName(NatMappingType::EndpointDependent), "endpoint_dependent");
    EXPECT_STREQ(dinero::p2p::NatMappingName(NatMappingType::Unknown), "unknown");
}
