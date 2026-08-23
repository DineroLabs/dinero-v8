#pragma once

#include "p2p/outbound_policy.h"
#include "p2p/peer_quality.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero {
namespace p2p {

struct PeerGovernorConfig {
    size_t target_hot_outbound{kTargetDurableOutbound};
    size_t target_warm_standby{16};
    size_t target_relay_registrations{3};
    size_t max_configured_seed_hot{kMandatoryAnchorOutbound};
    int demote_score_threshold{35};
};

struct PeerGovernorCandidate {
    std::string endpoint;
    PeerQualitySnapshot quality;
    bool connected{false};
    bool outbound{false};
    bool configured_seed{false};
    // Mandatory recovery anchors may be scored and reported, but active
    // slow-churn must never nominate them for disconnection.
    bool protected_anchor{false};
    bool relay_capable{false};
};

struct PeerGovernorDecision {
    std::vector<std::string> hot_peers;
    std::vector<std::string> warm_candidates;
    std::vector<std::string> relay_registration_candidates;
    std::vector<std::string> demote_candidates;
    size_t connected_outbound{0};
    size_t configured_seed_hot{0};
    size_t relay_capable_seen{0};
};

class PeerGovernor {
public:
    explicit PeerGovernor(PeerGovernorConfig config = {});

    // Dry-run policy evaluation. This method has no side effects and never
    // dials, disconnects, rotates, or registers peers.
    PeerGovernorDecision Evaluate(const std::vector<PeerGovernorCandidate>& candidates) const;

private:
    PeerGovernorConfig config_;
};

} // namespace p2p
} // namespace dinero
