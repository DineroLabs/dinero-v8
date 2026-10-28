#include "p2p/hole_punch.h"

#include <algorithm>

namespace dinero::p2p {

namespace {

// Same key QuicEndpoint uses: family plus address bytes, port ignored.
std::array<uint8_t, 17> IpKey(const network::UdpAddr& a) {
    std::array<uint8_t, 17> k{};
    k[0] = static_cast<uint8_t>(a.family);
    const size_t n = a.family == network::UdpAddr::Family::V4 ? 4 : 16;
    std::copy_n(a.ip.begin(), n, k.begin() + 1);
    return k;
}

}  // namespace

std::vector<network::UdpAddr> ChoosePunchTargets(const PunchCandidates& local,
                                                 const PunchCandidates& remote) {
    std::vector<network::UdpAddr> out;
    if (!local.v6.empty()) {
        for (const auto& a : remote.v6) {
            if (out.size() >= kPunchMaxTargets) return out;
            out.push_back(a);
        }
    }
    if (local.v4 && remote.v4 && local.v4_mapping != NatMappingType::EndpointDependent &&
        remote.v4_mapping != NatMappingType::EndpointDependent &&
        IpKey(*local.v4) != IpKey(*remote.v4) && out.size() < kPunchMaxTargets) {
        out.push_back(*remote.v4);
    }
    return out;
}

PunchRole RoleFor(bool we_dialed_relayed, bool they_dialed_relayed_too,
                  const std::array<uint8_t, 20>& ours, const std::array<uint8_t, 20>& theirs) {
    if (we_dialed_relayed && !they_dialed_relayed_too) return PunchRole::Initiator;
    if (!we_dialed_relayed && they_dialed_relayed_too) return PunchRole::Acceptor;
    if (!we_dialed_relayed) return PunchRole::None;
    return ours < theirs ? PunchRole::Initiator : PunchRole::Acceptor;
}

bool PunchScheduler::MayStart(const PeerId& peer) {
    if (in_flight_.size() >= kPunchMaxInFlight) return false;
    const auto it = failed_at_.find(peer);
    return it == failed_at_.end() || clock_() - it->second >= kPunchFailureBackoff;
}

void PunchScheduler::Started(const PeerId& peer) { in_flight_.insert(peer); }

void PunchScheduler::Succeeded(const PeerId& peer) {
    in_flight_.erase(peer);
    failed_at_.erase(peer);
}

void PunchScheduler::Failed(const PeerId& peer) {
    in_flight_.erase(peer);
    failed_at_[peer] = clock_();
}

}  // namespace dinero::p2p
