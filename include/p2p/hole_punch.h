#pragma once

// Relay-coordinated hole punching, the decision half: which of the remote
// peer's addresses to probe, who plays which role, and how many punches may
// run at once / how soon a failed peer is retried.
//
// IPv6 first: it needs no NAT mapping, only a firewall pinhole that our own
// outbound probe opens. IPv4 is tried only when both sides' mappings are
// punchable (an endpoint-dependent NAT gives each destination its own port, so
// the address the relay saw is useless) and the two public IPs differ (peers
// behind one router would punch a hairpin the router usually drops).
//
// Pure logic: no sockets; the clock is injected as in SelfReachability.

#include "p2p/nat_classifier.h"

#include "network/udp_socket.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace dinero::p2p {

constexpr std::chrono::seconds kPunchAttemptTimeout{10};
constexpr std::chrono::milliseconds kPunchProbeInterval{250};
constexpr size_t kPunchMaxInFlight = 2;
constexpr std::chrono::minutes kPunchFailureBackoff{30};
constexpr size_t kPunchMaxTargets = 3;

struct PunchCandidates {
    std::vector<network::UdpAddr> v6;
    std::optional<network::UdpAddr> v4;
    NatMappingType v4_mapping{NatMappingType::Unknown};
};

// Wire payload shared by punchoff and punchans:
//   nonce u64 LE | v4_mapping u8 (0 unknown, 1 independent, 2 dependent)
//   | count u8 (<= 8) | count x (net u8 [BIP155 1=v4, 2=v6] | addr 4/16 | port u16 BE)
// At most one v4 entry (written first). Anything else, trailing bytes
// included, is rejected whole.
std::vector<uint8_t> EncodePunchCandidates(uint64_t nonce, const PunchCandidates& c);
std::optional<std::pair<uint64_t, PunchCandidates>> ParsePunchCandidates(
    const std::vector<uint8_t>& payload);

// Remote addresses to probe, in order, at most kPunchMaxTargets.
std::vector<network::UdpAddr> ChoosePunchTargets(const PunchCandidates& local,
                                                 const PunchCandidates& remote);

enum class PunchRole { None, Initiator, Acceptor };

// Who opens the QUIC handshake once both sides are probing. When both of us
// dialed through the relay the lower node id initiates, so exactly one
// handshake starts.
PunchRole RoleFor(bool we_dialed_relayed, bool they_dialed_relayed_too,
                  const std::array<uint8_t, 20>& ours, const std::array<uint8_t, 20>& theirs);

class PunchScheduler {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using PeerId = std::array<uint8_t, 20>;

    explicit PunchScheduler(Clock clock) : clock_(std::move(clock)) {}

    bool MayStart(const PeerId& peer);
    void Started(const PeerId& peer);
    void Succeeded(const PeerId& peer);
    void Failed(const PeerId& peer);
#ifdef DINERO_TEST_BUILD
    size_t test_failed_peer_count() const { return failed_at_.size(); }
#endif

private:
    Clock clock_;
    std::set<PeerId> in_flight_;
    std::map<PeerId, std::chrono::steady_clock::time_point> failed_at_;
};

}  // namespace dinero::p2p
