#include "p2p/nat_classifier.h"

#include <set>

namespace dinero::p2p {

NatMapping Classify(uint16_t local_port,
                    const std::vector<std::pair<network::UdpAddr, network::UdpAddr>>& observations) {
    using network::UdpAddr;
    NatMapping out;
    std::set<std::array<uint8_t, 4>> servers;
    std::set<std::pair<std::array<uint8_t, 4>, uint16_t>> mappings;
    bool all_kept_port = true;
    for (const auto& [server, refl] : observations) {
        if (server.family != UdpAddr::Family::V4 || refl.family != UdpAddr::Family::V4) continue;
        servers.insert({server.ip[0], server.ip[1], server.ip[2], server.ip[3]});
        mappings.insert({{refl.ip[0], refl.ip[1], refl.ip[2], refl.ip[3]}, refl.port});
        if (refl.port != local_port) all_kept_port = false;
    }
    if (mappings.empty()) return out;
    out.port_preserving = all_kept_port;
    if (mappings.size() > 1) {
        out.mapping = NatMappingType::EndpointDependent;  // even one server can show this
    } else if (servers.size() >= 2) {
        out.mapping = NatMappingType::EndpointIndependent;
    }
    return out;
}

const char* NatMappingName(NatMappingType t) {
    switch (t) {
        case NatMappingType::EndpointIndependent: return "endpoint_independent";
        case NatMappingType::EndpointDependent: return "endpoint_dependent";
        case NatMappingType::Unknown: break;
    }
    return "unknown";
}

}  // namespace dinero::p2p
