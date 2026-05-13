#pragma once

#include <optional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <random>
#include "primitives/uint256.h"

namespace dinero {
namespace consensus {

/**
 * Peer proof capability classification
 * 
 * Phase 9.2: Used for intelligent peer selection
 */
enum class PeerProofCapability {
    UNKNOWN = 0,           // Not yet probed
    BRIDGE_NODE = 1,       // Advertises NODE_UTREEXO_BRIDGE (can generate proofs)
    STATELESS_NODE = 2,    // May have cached proofs (best-effort)
    STATEFUL_FULL = 3,     // Cannot provide proofs (UTXO database only)
};

/**
 * Peer proof request statistics
 * 
 * Phase 9.2: Track peer reliability for routing decisions
 * 
 * Non-consensus: These stats are local to each node and do not
 * affect validation. Only used for optimization.
 */
struct PeerProofStats {
    uint32_t requests_sent = 0;      // Total proof requests sent to this peer
    uint32_t proofs_received = 0;    // Successful proof deliveries
    uint32_t timeouts = 0;           // Requests that timed out
    uint32_t invalid_proofs = 0;     // Protocol errors (not consensus failures)
    uint64_t last_request_time = 0;  // Unix timestamp of last request
    uint64_t penalty_until = 0;      // Temporary ban until this timestamp

    /**
     * Calculate success rate (0.0 to 1.0)
     * Returns 0.0 if no requests have been sent
     */
    double SuccessRate() const {
        if (requests_sent == 0) return 0.0;
        return static_cast<double>(proofs_received) / requests_sent;
    }

    /**
     * Check if peer is currently penalized
     */
    bool IsPenalized() const;

    /**
     * Apply temporary penalty (30 seconds default)
     */
    void ApplyPenalty(uint64_t duration_secs = 30);

    /**
     * Clear penalty
     */
    void ClearPenalty() { penalty_until = 0; }
};

/**
 * Peer information for routing decisions
 */
struct PeerInfo {
    uint64_t peer_id;                         // Unique peer identifier
    PeerProofCapability capability;           // What can this peer provide?
    uint256 chain_tip;                        // Peer's current chain tip
    uint32_t chain_height;                    // Peer's current chain height
    PeerProofStats stats;                     // Request/response statistics

    PeerInfo() : peer_id(0), capability(PeerProofCapability::UNKNOWN), chain_height(0) {}

    PeerInfo(uint64_t id, PeerProofCapability cap)
        : peer_id(id), capability(cap), chain_height(0) {}
};

/**
 * Proof request routing engine
 * 
 * Phase 9.2: Non-consensus peer selection for proof requests
 * 
 * Design principles:
 * - Local decisions (each node chooses independently)
 * - Non-deterministic (can vary by time/network state)
 * - Non-consensus-visible (cannot affect validation)
 * 
 * Routing strategy:
 * 1. Prefer NODE_UTREEXO_BRIDGE peers (can generate proofs)
 * 2. Prefer peers with matching chain tip (likely have proof)
 * 3. Penalize peers with repeated failures (temporary ban)
 * 4. Round-robin among remaining candidates
 * 
 * Thread safety:
 * - All operations are mutex-protected
 */
class ProofRouter {
public:
    ProofRouter();
    ~ProofRouter() = default;

    /**
     * Register a peer for routing consideration
     * 
     * @param peer_id Unique peer identifier
     * @param capability Peer's proof capability
     */
    void RegisterPeer(uint64_t peer_id, PeerProofCapability capability);

    /**
     * Unregister a peer (disconnected)
     * 
     * @param peer_id Peer to remove
     */
    void UnregisterPeer(uint64_t peer_id);

    /**
     * Update peer's chain state
     * 
     * @param peer_id Peer to update
     * @param tip_hash Chain tip hash
     * @param height Chain height
     */
    void UpdatePeerChainState(uint64_t peer_id, const uint256& tip_hash, uint32_t height);

    /**
     * Select peer for proof request
     * 
     * Phase 9.2: Routing heuristics (non-consensus, non-deterministic)
     * 
     * Strategy:
     * 1. Filter out penalized peers
     * 2. Prefer bridge nodes over stateless nodes
     * 3. Among same capability, prefer matching chain tip
     * 4. Among same tier, round-robin (introduces non-determinism)
     * 
     * @param block_hash Block whose proof we need
     * @param block_height Height of block (for chain state matching)
     * @return Peer ID if candidate found, std::nullopt if no peers available
     */
    std::optional<uint64_t> SelectPeerForProof(const uint256& block_hash, uint32_t block_height);

    /**
     * Record successful proof delivery
     * 
     * @param peer_id Peer that delivered proof
     */
    void RecordSuccess(uint64_t peer_id);

    /**
     * Record timeout
     * 
     * @param peer_id Peer that timed out
     * @param apply_penalty If true, temporarily ban this peer
     */
    void RecordTimeout(uint64_t peer_id, bool apply_penalty = true);

    /**
     * Record protocol error (invalid proof format, not consensus failure)
     * 
     * @param peer_id Peer that sent invalid protocol message
     * @param apply_penalty If true, temporarily ban this peer
     */
    void RecordProtocolError(uint64_t peer_id, bool apply_penalty = true);

    /**
     * Get peer statistics (for monitoring/debugging)
     */
    std::optional<PeerProofStats> GetPeerStats(uint64_t peer_id) const;

    /**
     * Get all registered peers (for testing)
     */
    std::vector<uint64_t> GetAllPeers() const;

    /**
     * Clear all routing state (for testing)
     */
    void Clear();

private:
    // Peer registry
    std::unordered_map<uint64_t, PeerInfo> peers_;

    // Random number generator (for non-deterministic round-robin)
    std::mt19937 rng_;

    // Thread safety
    mutable std::mutex mutex_;

    // Helper: Get viable candidates for proof request
    std::vector<uint64_t> GetViableCandidates(const uint256& block_hash, uint32_t block_height);

    // Helper: Select from candidates using routing heuristics
    std::optional<uint64_t> SelectFromCandidates(const std::vector<uint64_t>& candidates);

    // Helper: Check if peer is viable (not penalized, has capability)
    bool IsPeerViable(const PeerInfo& peer) const;
};

} // namespace consensus
} // namespace dinero
