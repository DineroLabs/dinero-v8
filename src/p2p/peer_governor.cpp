#include "p2p/peer_governor.h"

#include <algorithm>

namespace dinero {
namespace p2p {

namespace {

bool BetterQuality(const PeerGovernorCandidate* lhs,
                   const PeerGovernorCandidate* rhs) {
    if (lhs->quality.score != rhs->quality.score) {
        return lhs->quality.score > rhs->quality.score;
    }
    if (lhs->quality.latency_ms != rhs->quality.latency_ms) {
        if (lhs->quality.latency_ms == 0) return false;
        if (rhs->quality.latency_ms == 0) return true;
        return lhs->quality.latency_ms < rhs->quality.latency_ms;
    }
    return lhs->endpoint < rhs->endpoint;
}

} // namespace

PeerGovernor::PeerGovernor(PeerGovernorConfig config)
    : config_(config) {}

PeerGovernorDecision PeerGovernor::Evaluate(
    const std::vector<PeerGovernorCandidate>& candidates) const {
    PeerGovernorDecision decision;
    std::vector<const PeerGovernorCandidate*> connected_outbound;
    std::vector<const PeerGovernorCandidate*> warm;
    std::vector<const PeerGovernorCandidate*> relays;

    for (const auto& candidate : candidates) {
        if (candidate.relay_capable) {
            ++decision.relay_capable_seen;
        }

        if (candidate.connected && candidate.outbound) {
            ++decision.connected_outbound;
            connected_outbound.push_back(&candidate);
            if (!candidate.protected_anchor &&
                candidate.quality.score <= config_.demote_score_threshold) {
                decision.demote_candidates.push_back(candidate.endpoint);
            }
        } else if (!candidate.connected && candidate.quality.hot_peer_candidate) {
            warm.push_back(&candidate);
        }

        if (candidate.relay_capable && candidate.quality.relay_candidate) {
            relays.push_back(&candidate);
        }
    }

    std::sort(connected_outbound.begin(), connected_outbound.end(), BetterQuality);
    std::sort(warm.begin(), warm.end(), BetterQuality);
    std::sort(relays.begin(), relays.end(), BetterQuality);

    size_t configured_seed_hot = 0;
    for (const auto* candidate : connected_outbound) {
        if (decision.hot_peers.size() >= config_.target_hot_outbound) {
            break;
        }
        if (candidate->configured_seed &&
            configured_seed_hot >= config_.max_configured_seed_hot) {
            continue;
        }
        decision.hot_peers.push_back(candidate->endpoint);
        if (candidate->configured_seed) {
            ++configured_seed_hot;
        }
    }
    decision.configured_seed_hot = configured_seed_hot;

    for (const auto* candidate : warm) {
        if (decision.warm_candidates.size() >= config_.target_warm_standby) {
            break;
        }
        decision.warm_candidates.push_back(candidate->endpoint);
    }

    for (const auto* candidate : relays) {
        if (decision.relay_registration_candidates.size() >=
            config_.target_relay_registrations) {
            break;
        }
        decision.relay_registration_candidates.push_back(candidate->endpoint);
    }

    std::sort(decision.demote_candidates.begin(), decision.demote_candidates.end());
    return decision;
}

} // namespace p2p
} // namespace dinero
