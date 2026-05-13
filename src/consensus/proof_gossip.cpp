#include "consensus/proof_gossip.h"
#include "common/sha256d.h"
#include <chrono>
#include <cstring>

namespace dinero {
namespace consensus {

// InvProof implementation

std::vector<uint8_t> InvProof::Serialize() const {
    std::vector<uint8_t> data;
    data.reserve(32 + 4 + 32 + 1);  // block_hash + proof_size + proof_hash + ttl

    // block_hash (32 bytes)
    data.insert(data.end(), block_hash.data, block_hash.data + 32);

    // proof_size (4 bytes, little-endian)
    data.push_back(static_cast<uint8_t>(proof_size & 0xFF));
    data.push_back(static_cast<uint8_t>((proof_size >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((proof_size >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((proof_size >> 24) & 0xFF));

    // proof_hash (32 bytes)
    data.insert(data.end(), proof_hash.data, proof_hash.data + 32);

    // ttl (1 byte)
    data.push_back(ttl);

    return data;
}

InvProof InvProof::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 69) {  // 32 + 4 + 32 + 1
        return InvProof();  // Invalid
    }

    InvProof inv;

    // block_hash
    std::memcpy(inv.block_hash.data, data.data(), 32);

    // proof_size
    inv.proof_size = static_cast<uint32_t>(data[32]) |
                     (static_cast<uint32_t>(data[33]) << 8) |
                     (static_cast<uint32_t>(data[34]) << 16) |
                     (static_cast<uint32_t>(data[35]) << 24);

    // proof_hash
    std::memcpy(inv.proof_hash.data, data.data() + 36, 32);

    // ttl
    inv.ttl = data[68];

    return inv;
}

// GetProof implementation

std::vector<uint8_t> GetProof::Serialize() const {
    std::vector<uint8_t> data;
    data.reserve(64);  // block_hash + expected_root

    // block_hash (32 bytes)
    data.insert(data.end(), block_hash.data, block_hash.data + 32);

    // expected_root (32 bytes)
    data.insert(data.end(), expected_root.data, expected_root.data + 32);

    return data;
}

GetProof GetProof::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 64) {
        return GetProof();  // Invalid
    }

    GetProof req;

    // block_hash
    std::memcpy(req.block_hash.data, data.data(), 32);

    // expected_root
    std::memcpy(req.expected_root.data, data.data() + 32, 32);

    return req;
}

// ProofData implementation

std::vector<uint8_t> ProofData::Serialize() const {
    std::vector<uint8_t> data;

    // block_hash (32 bytes)
    data.insert(data.end(), block_hash.data, block_hash.data + 32);

    // proof data (variable length)
    auto proof_bytes = proof.serialize();
    data.insert(data.end(), proof_bytes.begin(), proof_bytes.end());

    return data;
}

ProofData ProofData::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 32) {
        return ProofData();  // Invalid
    }

    ProofData pd;

    // block_hash
    std::memcpy(pd.block_hash.data, data.data(), 32);

    // proof data
    std::vector<uint8_t> proof_bytes(data.begin() + 32, data.end());
    pd.proof = BlockUtreexoData::deserialize(proof_bytes);

    return pd;
}

// GossipTracker implementation

GossipTracker::GossipTracker() {
    // Initialize empty
}

uint64_t GossipTracker::GetCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool GossipTracker::RecordInvProof(const uint256& proof_hash, uint64_t peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if we've already seen this invproof
    if (seen_invproofs_.find(proof_hash) != seen_invproofs_.end()) {
        return false;  // Duplicate
    }

    // Record it
    seen_invproofs_[proof_hash] = GetCurrentTimestamp();
    return true;
}

bool GossipTracker::HaveSeenInvProof(const uint256& proof_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_invproofs_.find(proof_hash) != seen_invproofs_.end();
}

void GossipTracker::RecordProofRequest(const uint256& block_hash, uint64_t peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    RequestInfo info;
    info.peer_id = peer_id;
    info.timestamp = GetCurrentTimestamp();

    proof_requests_[block_hash] = info;
}

bool GossipTracker::HaveRequestedProof(const uint256& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return proof_requests_.find(block_hash) != proof_requests_.end();
}

void GossipTracker::ClearProofRequest(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    proof_requests_.erase(block_hash);
}

void GossipTracker::Cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t now = GetCurrentTimestamp();

    // Clean up old invproofs
    for (auto it = seen_invproofs_.begin(); it != seen_invproofs_.end(); ) {
        if (now - it->second > TRACKING_TTL_SECS) {
            it = seen_invproofs_.erase(it);
        } else {
            ++it;
        }
    }

    // Clean up old proof requests
    for (auto it = proof_requests_.begin(); it != proof_requests_.end(); ) {
        if (now - it->second.timestamp > TRACKING_TTL_SECS) {
            it = proof_requests_.erase(it);
        } else {
            ++it;
        }
    }
}

void GossipTracker::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    seen_invproofs_.clear();
    proof_requests_.clear();
}

// ProofGossipManager implementation

ProofGossipManager::ProofGossipManager() {
    // Initialize empty
}

uint64_t ProofGossipManager::GetCurrentTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ProofGossipManager::RootMatchesExpected(
    const BlockUtreexoData& proof,
    const uint256& expected_root
) {
    if (expected_root.IsNull()) {
        return true;
    }
    if (proof.accumulator_root_before.size() != 32) {
        return false;
    }
    return std::memcmp(
        proof.accumulator_root_before.data(),
        expected_root.data,
        32
    ) == 0;
}

std::string ProofGossipManager::MakeInflightKey(
    const uint256& block_hash,
    const uint256& expected_root
) {
    std::string key = block_hash.GetHex();
    key.push_back(':');
    key.append(expected_root.GetHex());
    return key;
}

void ProofGossipManager::TouchCacheEntryLocked(const uint256& block_hash) {
    auto it = recent_proof_lru_lookup_.find(block_hash);
    if (it == recent_proof_lru_lookup_.end()) {
        return;
    }
    recent_proof_lru_.splice(recent_proof_lru_.begin(), recent_proof_lru_, it->second);
    it->second = recent_proof_lru_.begin();
}

void ProofGossipManager::EraseCachedProofLocked(const uint256& block_hash) {
    recent_proof_cache_.erase(block_hash);
    auto lru_it = recent_proof_lru_lookup_.find(block_hash);
    if (lru_it != recent_proof_lru_lookup_.end()) {
        recent_proof_lru_.erase(lru_it->second);
        recent_proof_lru_lookup_.erase(lru_it);
    }
}

void ProofGossipManager::CacheProofLocked(const ProofData& data, uint64_t now) {
    auto it = recent_proof_cache_.find(data.block_hash);
    if (it != recent_proof_cache_.end()) {
        it->second.data = data;
        it->second.cached_at = now;
        TouchCacheEntryLocked(data.block_hash);
        return;
    }

    while (recent_proof_cache_.size() >= MAX_RECENT_PROOF_ENTRIES && !recent_proof_lru_.empty()) {
        const uint256 evict_hash = recent_proof_lru_.back();
        EraseCachedProofLocked(evict_hash);
    }

    RecentProofEntry entry;
    entry.data = data;
    entry.cached_at = now;
    recent_proof_cache_.emplace(data.block_hash, std::move(entry));
    recent_proof_lru_.push_front(data.block_hash);
    recent_proof_lru_lookup_[data.block_hash] = recent_proof_lru_.begin();
}

std::optional<ProofData> ProofGossipManager::GetCachedProofLocked(
    const uint256& block_hash,
    const uint256& expected_root,
    uint64_t now
) {
    auto it = recent_proof_cache_.find(block_hash);
    if (it == recent_proof_cache_.end()) {
        return std::nullopt;
    }

    if (now > it->second.cached_at &&
        (now - it->second.cached_at) > RECENT_PROOF_TTL_SECS) {
        EraseCachedProofLocked(block_hash);
        return std::nullopt;
    }

    if (!RootMatchesExpected(it->second.data.proof, expected_root)) {
        return std::nullopt;
    }

    TouchCacheEntryLocked(block_hash);
    return it->second.data;
}

void ProofGossipManager::CleanupCachedProofsLocked(uint64_t now) {
    std::vector<uint256> stale;
    stale.reserve(recent_proof_cache_.size());
    for (const auto& [block_hash, entry] : recent_proof_cache_) {
        if (now > entry.cached_at &&
            (now - entry.cached_at) > RECENT_PROOF_TTL_SECS) {
            stale.push_back(block_hash);
        }
    }

    for (const auto& block_hash : stale) {
        EraseCachedProofLocked(block_hash);
    }
}

InvProof ProofGossipManager::AnnounceProof(const uint256& block_hash, const BlockUtreexoData& proof) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Calculate a canonical digest for deduplication using double-SHA256.
    auto proof_bytes = proof.serialize();
    uint256 proof_hash;
    const auto digest = Dinero::Common::double_sha256_raw(proof_bytes.data(), proof_bytes.size());
    if (digest.size() >= 32) {
        std::memcpy(proof_hash.data, digest.data(), 32);
    } else {
        std::memset(proof_hash.data, 0, sizeof(proof_hash.data));
    }

    InvProof inv(block_hash, static_cast<uint32_t>(proof_bytes.size()), proof_hash);

    CacheProofLocked(ProofData(block_hash, proof), GetCurrentTimestamp());
    stats_.invproofs_sent++;

    return inv;
}

void ProofGossipManager::PrewarmProof(const uint256& block_hash, const BlockUtreexoData& proof) {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheProofLocked(ProofData(block_hash, proof), GetCurrentTimestamp());
    stats_.proof_prewarmed++;
}

bool ProofGossipManager::HandleInvProof(const InvProof& inv, uint64_t peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    stats_.invproofs_received++;

    // Check if we've already seen this invproof
    if (!tracker_.RecordInvProof(inv.proof_hash, peer_id)) {
        stats_.invproofs_duplicate++;
        return false;  // Duplicate - ignore
    }

    // Check if we already have this proof or requested it
    if (tracker_.HaveRequestedProof(inv.block_hash)) {
        return false;  // Already requested - ignore
    }

    // We might want this proof
    return true;
}

GetProof ProofGossipManager::CreateProofRequest(const uint256& block_hash, const uint256& expected_root) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Record that we're requesting this
    tracker_.RecordProofRequest(block_hash, 0);  // peer_id = 0 for local requests

    stats_.proofs_requested++;

    return GetProof(block_hash, expected_root);
}

std::optional<ProofData> ProofGossipManager::HandleProofRequest(const GetProof& req, uint64_t peer_id) {
    (void)peer_id;

    std::shared_ptr<InflightProofRequest> state;
    std::function<std::optional<BlockUtreexoData>(const uint256&)> provider;
    bool owner = false;
    const std::string inflight_key = MakeInflightKey(req.block_hash, req.expected_root);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t now = GetCurrentTimestamp();
        CleanupCachedProofsLocked(now);

        auto cached = GetCachedProofLocked(req.block_hash, req.expected_root, now);
        if (cached.has_value()) {
            stats_.proof_cache_hits++;
            stats_.proofs_delivered++;
            return cached;
        }

        stats_.proof_cache_misses++;

        auto inflight_it = inflight_proof_requests_.find(inflight_key);
        if (inflight_it != inflight_proof_requests_.end()) {
            state = inflight_it->second;
            stats_.proof_requests_coalesced++;
        } else {
            if (!proof_provider_) {
                stats_.proof_provider_failures++;
                return std::nullopt;  // No provider configured
            }
            state = std::make_shared<InflightProofRequest>();
            inflight_proof_requests_[inflight_key] = state;
            provider = proof_provider_;
            owner = true;
        }
    }

    {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        state->waiters++;
    }

    if (owner) {
        std::optional<ProofData> result;
        try {
            auto proof = provider(req.block_hash);
            if (proof.has_value() && RootMatchesExpected(proof.value(), req.expected_root)) {
                result = ProofData(req.block_hash, proof.value());

                std::lock_guard<std::mutex> lock(mutex_);
                CacheProofLocked(result.value(), GetCurrentTimestamp());
            } else {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.proof_provider_failures++;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.proof_provider_failures++;
            result = std::nullopt;
        }

        {
            std::lock_guard<std::mutex> state_lock(state->mutex);
            state->result = std::move(result);
            state->done = true;
        }
        state->cv.notify_all();
    } else {
        std::unique_lock<std::mutex> state_lock(state->mutex);
        state->cv.wait(state_lock, [&state]() {
            return state->done;
        });
    }

    std::optional<ProofData> response;
    bool remove_inflight = false;
    {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        response = state->result;
        if (state->waiters > 0) {
            state->waiters--;
        }
        remove_inflight = state->done && state->waiters == 0;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (remove_inflight) {
            auto inflight_it = inflight_proof_requests_.find(inflight_key);
            if (inflight_it != inflight_proof_requests_.end() && inflight_it->second == state) {
                inflight_proof_requests_.erase(inflight_it);
            }
        }

        if (response.has_value() &&
            !RootMatchesExpected(response->proof, req.expected_root)) {
            response = std::nullopt;
        }

        if (response.has_value()) {
            stats_.proofs_delivered++;
        }
    }

    return response;
}

bool ProofGossipManager::HandleProofData(const ProofData& data, uint64_t peer_id) {
    (void)peer_id;
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if we were expecting this proof
    if (!tracker_.HaveRequestedProof(data.block_hash)) {
        if (recent_proof_cache_.find(data.block_hash) != recent_proof_cache_.end()) {
            stats_.proofdata_replayed++;
        } else {
            stats_.proofdata_unsolicited++;
        }
        return false;  // Unsolicited proof - ignore
    }

    // Clear request tracking
    tracker_.ClearProofRequest(data.block_hash);

    CacheProofLocked(data, GetCurrentTimestamp());
    stats_.proofs_received++;

    return true;
}

void ProofGossipManager::SetProofProvider(std::function<std::optional<BlockUtreexoData>(const uint256&)> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    proof_provider_ = provider;
}

void ProofGossipManager::PeriodicCleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracker_.Cleanup();
    CleanupCachedProofsLocked(GetCurrentTimestamp());
}

void ProofGossipManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracker_.Clear();
    stats_ = GossipStats();
    proof_provider_ = nullptr;
    recent_proof_cache_.clear();
    recent_proof_lru_.clear();
    recent_proof_lru_lookup_.clear();
    inflight_proof_requests_.clear();
}

ProofGossipManager::GossipStats ProofGossipManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GossipStats snapshot = stats_;
    snapshot.proof_cache_entries = static_cast<uint64_t>(recent_proof_cache_.size());
    snapshot.proof_cache_capacity = static_cast<uint64_t>(MAX_RECENT_PROOF_ENTRIES);
    snapshot.proof_cache_ttl_seconds = static_cast<uint64_t>(RECENT_PROOF_TTL_SECS);
    snapshot.inflight_requests = static_cast<uint64_t>(inflight_proof_requests_.size());
    return snapshot;
}

void ProofGossipManager::RecordInvalidGetProofPayload() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.invalid_getproof_payloads++;
}

void ProofGossipManager::RecordInvalidProofDataPayload() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.invalid_proofdata_payloads++;
}

void ProofGossipManager::RecordGetProofRateLimited() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.getproof_rate_limited++;
}

void ProofGossipManager::RecordProofDataRateLimited() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.proofdata_rate_limited++;
}

void ProofGossipManager::RecordPeerDisconnectedForAbuse() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.peers_disconnected_for_abuse++;
}

} // namespace consensus
} // namespace dinero
