#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include "primitives/uint256.h"
#include "primitives/block.h"

namespace dinero {
namespace consensus {

/**
 * Proof inventory announcement (gossip layer)
 * 
 * Phase 9.3: Best-effort proof availability announcement
 * 
 * Non-consensus: Missing invproof never causes block rejection.
 * Gossip is optimization for reducing redundant requests.
 */
struct InvProof {
    uint256 block_hash;      // Block this proof validates
    uint32_t proof_size;     // Size in bytes (for bandwidth planning)
    uint256 proof_hash;      // Hash for deduplication
    uint8_t ttl;             // Remaining gossip hops (max 2)

    static constexpr uint8_t MAX_TTL = 2;  // Max 2 hops to prevent flooding

    InvProof() : proof_size(0), ttl(MAX_TTL) {}

    InvProof(const uint256& block, uint32_t size, const uint256& hash, uint8_t hops = MAX_TTL)
        : block_hash(block), proof_size(size), proof_hash(hash), ttl(hops) {}

    /**
     * Check if this invproof can be re-gossiped
     */
    bool CanReGossip() const { return ttl > 0; }

    /**
     * Create decremented copy for re-gossip
     */
    InvProof DecrementTTL() const {
        return InvProof(block_hash, proof_size, proof_hash, ttl > 0 ? ttl - 1 : 0);
    }

    /**
     * Serialize to bytes (for network transmission)
     */
    std::vector<uint8_t> Serialize() const;

    /**
     * Deserialize from bytes
     */
    static InvProof Deserialize(const std::vector<uint8_t>& data);
};

/**
 * Proof request message
 * 
 * Phase 9.3: Request proof from peer after seeing invproof
 */
struct GetProof {
    uint256 block_hash;      // Which block's proof
    uint256 expected_root;   // For sanity check (optional, can be zero)

    GetProof() = default;

    GetProof(const uint256& block, const uint256& root = uint256())
        : block_hash(block), expected_root(root) {}

    std::vector<uint8_t> Serialize() const;
    static GetProof Deserialize(const std::vector<uint8_t>& data);
};

/**
 * Proof response message
 * 
 * Phase 9.3: Deliver proof to requesting peer
 */
struct ProofData {
    uint256 block_hash;           // Which block
    BlockUtreexoData proof;       // The actual proof (from Phase 7)

    ProofData() = default;

    ProofData(const uint256& block, const BlockUtreexoData& p)
        : block_hash(block), proof(p) {}

    std::vector<uint8_t> Serialize() const;
    static ProofData Deserialize(const std::vector<uint8_t>& data);
};

/**
 * Gossip message tracker
 * 
 * Phase 9.3: Track which invproofs we've seen to prevent spam
 */
class GossipTracker {
public:
    GossipTracker();
    ~GossipTracker() = default;

    /**
     * Record that we've seen this invproof
     * Returns false if we've already seen it (duplicate)
     */
    bool RecordInvProof(const uint256& proof_hash, uint64_t peer_id);

    /**
     * Check if we've already seen this invproof
     */
    bool HaveSeenInvProof(const uint256& proof_hash) const;

    /**
     * Record proof request (to avoid duplicate requests)
     */
    void RecordProofRequest(const uint256& block_hash, uint64_t peer_id);

    /**
     * Check if we've already requested this proof
     */
    bool HaveRequestedProof(const uint256& block_hash) const;

    /**
     * Clear request tracking (proof received or timed out)
     */
    void ClearProofRequest(const uint256& block_hash);

    /**
     * Periodic cleanup (remove old entries)
     */
    void Cleanup();

    /**
     * Clear all tracking state (for testing)
     */
    void Clear();

private:
    // Track seen invproofs (proof_hash -> timestamp)
    std::unordered_map<uint256, uint64_t> seen_invproofs_;

    // Track proof requests (block_hash -> peer_id + timestamp)
    struct RequestInfo {
        uint64_t peer_id;
        uint64_t timestamp;
    };
    std::unordered_map<uint256, RequestInfo> proof_requests_;

    // TTL for tracking (5 minutes)
    static constexpr uint64_t TRACKING_TTL_SECS = 300;

    // Thread safety
    mutable std::mutex mutex_;

    // Helper: Get current timestamp
    static uint64_t GetCurrentTimestamp();
};

/**
 * Proof gossip manager
 * 
 * Phase 9.3: Coordinate proof gossip protocol
 * 
 * Best-effort availability layer:
 * - Announce proofs we have (invproof)
 * - Request proofs we need (getproof)
 * - Deliver proofs (proofdata)
 * 
 * Non-consensus guarantees:
 * - Gossip failure never causes block rejection
 * - Missing invproof = normal operation (direct request fallback)
 * - TTL limit prevents network flooding
 */
class ProofGossipManager {
public:
    ProofGossipManager();
    ~ProofGossipManager() = default;

    /**
     * Announce that we have a proof (broadcast invproof)
     * 
     * @param block_hash Block hash
     * @param proof Proof data
     * @return InvProof message to broadcast
     */
    InvProof AnnounceProof(const uint256& block_hash, const BlockUtreexoData& proof);

    /**
     * Handle incoming invproof from peer
     * 
     * @param inv Received invproof
     * @param peer_id Peer who sent it
     * @return true if we should request this proof, false if duplicate/unwanted
     */
    bool HandleInvProof(const InvProof& inv, uint64_t peer_id);

    /**
     * Create getproof request
     * 
     * @param block_hash Block whose proof we need
     * @param expected_root Expected root (optional)
     * @return GetProof message to send
     */
    GetProof CreateProofRequest(const uint256& block_hash, const uint256& expected_root = uint256());

    /**
     * Handle incoming getproof request
     * 
     * @param req Received request
     * @param peer_id Peer who sent it
     * @return ProofData if we have it, nullopt if we don't
     */
    std::optional<ProofData> HandleProofRequest(const GetProof& req, uint64_t peer_id);

    /**
     * Handle incoming proofdata response
     * 
     * @param data Received proof
     * @param peer_id Peer who sent it
     * @return true if we were expecting this proof, false otherwise
     */
    bool HandleProofData(const ProofData& data, uint64_t peer_id);

    /**
     * Prewarm a freshly-generated proof into the recent cache.
     *
     * Used on fresh tip blocks so first getproof requests hit cache instead of
     * triggering on-demand provider work.
     */
    void PrewarmProof(const uint256& block_hash, const BlockUtreexoData& proof);

    /**
     * Set proof provider (function to retrieve proofs we have)
     * 
     * @param provider Callback: (block_hash) -> optional<BlockUtreexoData>
     */
    void SetProofProvider(std::function<std::optional<BlockUtreexoData>(const uint256&)> provider);

    /**
     * Periodic maintenance (cleanup old tracking data)
     */
    void PeriodicCleanup();

    /**
     * Clear all gossip state (for testing)
     */
    void Clear();

    /**
     * Get gossip statistics (for monitoring)
     */
    struct GossipStats {
        uint64_t invproofs_sent = 0;
        uint64_t invproofs_received = 0;
        uint64_t invproofs_duplicate = 0;
        uint64_t proofs_requested = 0;
        uint64_t proofs_delivered = 0;
        uint64_t proofs_received = 0;
        uint64_t proof_cache_hits = 0;
        uint64_t proof_cache_misses = 0;
        uint64_t proof_requests_coalesced = 0;
        uint64_t proof_provider_failures = 0;
        uint64_t proof_prewarmed = 0;
        uint64_t proofdata_unsolicited = 0;
        uint64_t proofdata_replayed = 0;
        uint64_t invalid_getproof_payloads = 0;
        uint64_t invalid_proofdata_payloads = 0;
        uint64_t getproof_rate_limited = 0;
        uint64_t proofdata_rate_limited = 0;
        uint64_t peers_disconnected_for_abuse = 0;
        uint64_t proof_cache_entries = 0;
        uint64_t proof_cache_capacity = 0;
        uint64_t proof_cache_ttl_seconds = 0;
        uint64_t inflight_requests = 0;
    };

    GossipStats GetStats() const;

    // Runtime instrumentation hooks (daemon P2P handler side).
    void RecordInvalidGetProofPayload();
    void RecordInvalidProofDataPayload();
    void RecordGetProofRateLimited();
    void RecordProofDataRateLimited();
    void RecordPeerDisconnectedForAbuse();

private:
    struct RecentProofEntry {
        ProofData data;
        uint64_t cached_at = 0;
    };

    struct InflightProofRequest {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::optional<ProofData> result;
        uint32_t waiters = 0;
    };

    static constexpr uint64_t RECENT_PROOF_TTL_SECS = 120;
    static constexpr size_t MAX_RECENT_PROOF_ENTRIES = 256;

    static uint64_t GetCurrentTimestamp();
    static bool RootMatchesExpected(
        const BlockUtreexoData& proof,
        const uint256& expected_root
    );
    static std::string MakeInflightKey(
        const uint256& block_hash,
        const uint256& expected_root
    );
    void CacheProofLocked(const ProofData& data, uint64_t now);
    std::optional<ProofData> GetCachedProofLocked(
        const uint256& block_hash,
        const uint256& expected_root,
        uint64_t now
    );
    void TouchCacheEntryLocked(const uint256& block_hash);
    void EraseCachedProofLocked(const uint256& block_hash);
    void CleanupCachedProofsLocked(uint64_t now);

    // Gossip tracker
    GossipTracker tracker_;

    // Statistics
    mutable GossipStats stats_;

    // Proof provider callback
    std::function<std::optional<BlockUtreexoData>(const uint256&)> proof_provider_;

    // Recent-proof cache for burst handling (block_hash -> proofdata).
    std::unordered_map<uint256, RecentProofEntry> recent_proof_cache_;
    std::list<uint256> recent_proof_lru_;
    std::unordered_map<uint256, std::list<uint256>::iterator> recent_proof_lru_lookup_;

    // In-flight request coalescing (block_hash -> single provider call).
    std::unordered_map<std::string, std::shared_ptr<InflightProofRequest>> inflight_proof_requests_;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace consensus
} // namespace dinero
