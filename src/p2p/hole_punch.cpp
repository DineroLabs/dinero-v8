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

constexpr size_t kPunchMaxWireEntries = 8;

std::vector<uint8_t> EncodePunchCandidates(uint64_t nonce, const PunchCandidates& c) {
    using Family = network::UdpAddr::Family;
    std::vector<uint8_t> out;
    for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(nonce >> (i * 8)));
    out.push_back(c.v4_mapping == NatMappingType::EndpointIndependent  ? 1
                  : c.v4_mapping == NatMappingType::EndpointDependent ? 2
                                                                      : 0);
    std::vector<const network::UdpAddr*> entries;
    if (c.v4 && c.v4->family == Family::V4) entries.push_back(&*c.v4);
    for (const auto& a : c.v6) {
        if (entries.size() >= kPunchMaxWireEntries) break;
        if (a.family == Family::V6) entries.push_back(&a);
    }
    out.push_back(static_cast<uint8_t>(entries.size()));
    for (const auto* a : entries) {
        out.push_back(static_cast<uint8_t>(a->family));  // 1 = v4, 2 = v6 (BIP155)
        out.insert(out.end(), a->ip.begin(), a->ip.begin() + (a->family == Family::V4 ? 4 : 16));
        out.push_back(static_cast<uint8_t>(a->port >> 8));
        out.push_back(static_cast<uint8_t>(a->port & 0xFF));
    }
    return out;
}

std::optional<std::pair<uint64_t, PunchCandidates>> ParsePunchCandidates(
    const std::vector<uint8_t>& payload) {
    constexpr size_t kHeader = 10;
    if (payload.size() < kHeader) return std::nullopt;
    uint64_t nonce = 0;
    for (int i = 0; i < 8; i++) nonce |= static_cast<uint64_t>(payload[i]) << (i * 8);

    PunchCandidates out;
    switch (payload[8]) {
        case 0: out.v4_mapping = NatMappingType::Unknown; break;
        case 1: out.v4_mapping = NatMappingType::EndpointIndependent; break;
        case 2: out.v4_mapping = NatMappingType::EndpointDependent; break;
        default: return std::nullopt;
    }
    const size_t count = payload[9];
    if (count > kPunchMaxWireEntries) return std::nullopt;

    size_t off = kHeader;
    for (size_t i = 0; i < count; i++) {
        if (off >= payload.size()) return std::nullopt;
        const uint8_t net = payload[off++];
        const size_t len = net == 1 ? 4 : net == 2 ? 16 : 0;
        if (len == 0 || off + len + 2 > payload.size()) return std::nullopt;
        const uint16_t port = static_cast<uint16_t>((payload[off + len] << 8) | payload[off + len + 1]);
        if (net == 1) {
            if (out.v4) return std::nullopt;  // at most one v4 entry
            out.v4 = network::UdpAddr::FromIPv4(&payload[off], port);
        } else {
            out.v6.push_back(network::UdpAddr::FromIPv6(&payload[off], port));
        }
        off += len + 2;
    }
    if (off != payload.size()) return std::nullopt;  // trailing bytes
    return std::make_pair(nonce, std::move(out));
}

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
