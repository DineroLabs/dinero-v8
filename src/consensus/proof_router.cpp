#include "consensus/proof_router.h"
#include <chrono>
#include <algorithm>

namespace dinero {
namespace consensus {

// PeerProofStats implementation

bool PeerProofStats::IsPenalized() const {
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return now < penalty_until;
}

void PeerProofStats::ApplyPenalty(uint64_t duration_secs) {
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    penalty_until = now + duration_secs;
}

// ProofRouter implementation

ProofRouter::ProofRouter() {
    // Initialize RNG with current time for non-deterministic behavior
    auto seed = std::chrono::system_clock::now().time_since_epoch().count();
    rng_.seed(static_cast<std::mt19937::result_type>(seed));
}

void ProofRouter::RegisterPeer(uint64_t peer_id, PeerProofCapability capability) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (peers_.find(peer_id) != peers_.end()) {
        // Already registered - update capability
        peers_[peer_id].capability = capability;
    } else {
        // New peer
        PeerInfo info(peer_id, capability);
        peers_[peer_id] = info;
    }
}

void ProofRouter::UnregisterPeer(uint64_t peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(peer_id);
}

void ProofRouter::UpdatePeerChainState(uint64_t peer_id, const uint256& tip_hash, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.chain_tip = tip_hash;
        it->second.chain_height = height;
    }
}

std::optional<uint64_t> ProofRouter::SelectPeerForProof(const uint256& block_hash, uint32_t block_height) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get viable candidates
    auto candidates = GetViableCandidates(block_hash, block_height);

    if (candidates.empty()) {
        return std::nullopt;
    }

    // Select using routing heuristics
    return SelectFromCandidates(candidates);
}

void ProofRouter::RecordSuccess(uint64_t peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.stats.requests_sent++;
        it->second.stats.proofs_received++;
        it->second.stats.last_request_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

void ProofRouter::RecordTimeout(uint64_t peer_id, bool apply_penalty) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.stats.requests_sent++;
        it->second.stats.timeouts++;
        it->second.stats.last_request_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (apply_penalty) {
            it->second.stats.ApplyPenalty(30);  // 30 second penalty
        }
    }
}

void ProofRouter::RecordProtocolError(uint64_t peer_id, bool apply_penalty) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second.stats.requests_sent++;
        it->second.stats.invalid_proofs++;
        it->second.stats.last_request_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (apply_penalty) {
            it->second.stats.ApplyPenalty(60);  // 60 second penalty for protocol errors
        }
    }
}

std::optional<PeerProofStats> ProofRouter::GetPeerStats(uint64_t peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        return it->second.stats;
    }

    return std::nullopt;
}

std::vector<uint64_t> ProofRouter::GetAllPeers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint64_t> result;
    result.reserve(peers_.size());

    for (const auto& [peer_id, info] : peers_) {
        result.push_back(peer_id);
    }

    return result;
}

void ProofRouter::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
}

// Private helpers

std::vector<uint64_t> ProofRouter::GetViableCandidates(const uint256& block_hash, uint32_t block_height) {
    std::vector<uint64_t> candidates;

    for (const auto& [peer_id, info] : peers_) {
        if (IsPeerViable(info)) {
            candidates.push_back(peer_id);
        }
    }

    return candidates;
}

std::optional<uint64_t> ProofRouter::SelectFromCandidates(const std::vector<uint64_t>& candidates) {
    if (candidates.empty()) {
        return std::nullopt;
    }

    // Categorize candidates by capability
    std::vector<uint64_t> bridge_nodes;
    std::vector<uint64_t> stateless_nodes;
    std::vector<uint64_t> other_nodes;

    for (uint64_t peer_id : candidates) {
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) continue;

        switch (it->second.capability) {
            case PeerProofCapability::BRIDGE_NODE:
                bridge_nodes.push_back(peer_id);
                break;
            case PeerProofCapability::STATELESS_NODE:
                stateless_nodes.push_back(peer_id);
                break;
            default:
                other_nodes.push_back(peer_id);
                break;
        }
    }

    // Routing strategy: Prefer bridge nodes > stateless nodes > others
    std::vector<uint64_t>* selected_tier = nullptr;

    if (!bridge_nodes.empty()) {
        selected_tier = &bridge_nodes;
    } else if (!stateless_nodes.empty()) {
        selected_tier = &stateless_nodes;
    } else if (!other_nodes.empty()) {
        selected_tier = &other_nodes;
    }

    if (selected_tier == nullptr || selected_tier->empty()) {
        return std::nullopt;
    }

    // Within tier: Round-robin with randomness (non-deterministic)
    std::uniform_int_distribution<size_t> dist(0, selected_tier->size() - 1);
    size_t index = dist(rng_);

    return (*selected_tier)[index];
}

bool ProofRouter::IsPeerViable(const PeerInfo& peer) const {
    // Peer must not be penalized
    if (peer.stats.IsPenalized()) {
        return false;
    }

    // Peer must have useful capability
    if (peer.capability == PeerProofCapability::STATEFUL_FULL) {
        // Full nodes cannot provide proofs
        return false;
    }

    return true;
}

} // namespace consensus
} // namespace dinero
