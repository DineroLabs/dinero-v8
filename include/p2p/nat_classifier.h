#pragma once

// NAT mapping behaviour (RFC 4787 §4.1) from one round of STUN answers taken
// on the P2P UDP socket.
//
// Endpoint-independent: the router gives our port one public mapping whatever
// the destination, so a peer told that address can reach us (hole punching
// works). Endpoint-dependent: each destination gets its own mapping, so the
// address one server saw is useless to anyone else. Two distinct server IPs
// are needed to tell them apart; with fewer the answer is Unknown.
//
// Only IPv4 answers are classified: an IPv6 server sees our v6 address, a
// different socket family rather than a second NAT mapping.

#include "network/udp_socket.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace dinero::p2p {

enum class NatMappingType { Unknown, EndpointIndependent, EndpointDependent };

struct NatMapping {
    NatMappingType mapping{NatMappingType::Unknown};
    bool port_preserving{false};  // every v4 answer kept our local port
};

// `observations`: (server, reflexive address that server reported).
NatMapping Classify(uint16_t local_port,
                    const std::vector<std::pair<network::UdpAddr, network::UdpAddr>>& observations);

const char* NatMappingName(NatMappingType t);

}  // namespace dinero::p2p
