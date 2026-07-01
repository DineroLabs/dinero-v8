/**
 * @file stateless_node.h
 * @brief Phase 7.3: Stateless Node Implementation
 *
 * Implements a stateless full node that syncs without storing the UTXO database.
 * Stateless nodes request Utreexo proofs from bridge nodes, validate them
 * cryptographically, and maintain only the compact Utreexo accumulator.
 *
 * **Key Features:**
 * - Headers-first sync (PoW verification before proof downloads)
 * - Root continuity enforcement (accumulator_root_before → accumulator_root_after)
 * - Parallel proof downloads from multiple bridge nodes
 * - No trust assumptions beyond PoW (all proofs cryptographically verified)
 * - Resource-efficient: ~100 MB accumulator vs ~5 GB UTXO database
 *
 * **Security Model:**
 * Stateless nodes do NOT trust bridge nodes. Every proof is verified against:
 * 1. Block hash matches proof message
 * 2. Root continuity: current_root == proof.root_before
 * 3. Utreexo batch proof verification
 * 4. Post-application root matches: accumulator_root == proof.root_after
 *
 * **Workflow:**
 * 1. Download headers (validate PoW)
 * 2. Select 8+ bridge nodes (diverse subnets, low latency)
 * 3. Request proofs in batches (max 16 blocks per request)
 * 4. Validate each proof with root continuity
 * 5. Apply block to accumulator
 * 6. Verify root_after matches
 * 7. Repeat until synced
 *
 * @see docs/PHASE_7_PROOF_SERVING_PROTOCOL.md
 */

#pragma once

#include "network/utreexo_messages.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_stump.h"
#include "consensus/utreexo_proof_relay.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include <functional>
#include <vector>
#include <optional>
#include <cstdint>
#include <map>
#include <set>
#include <chrono>
#include <unordered_map>

namespace dinero {

// Forward declaration
class PeerConnection;

namespace network {

/**
 * @brief Peer information for bridge node selection
 */
struct BridgePeer {
    uint64_t peer_id;           ///< Unique peer identifier
    std::string address;         ///< IP address or hostname
    uint16_t port;               ///< Port number
    uint64_t service_flags;      ///< Service flags (must have NODE_UTREEXO_BRIDGE)
    uint32_t latency_ms;         ///< Average latency in milliseconds
    std::string subnet;          ///< Subnet for diversity (/24 for IPv4)
    uint32_t proofs_requested;   ///< Number of proofs requested from this peer
    uint32_t proofs_received;    ///< Number of proofs received successfully
    uint32_t proofs_failed;      ///< Number of proof validation failures

    /**
     * @brief Calculate peer quality score (higher is better)
     */
    double GetQualityScore() const {
        if (proofs_requested == 0) return 1.0;
        // Laplace-smoothed success rate: penalizes peers with high failure rates
        double success_rate = static_cast<double>(proofs_received)
                            / (proofs_received + proofs_failed + 1.0);
        double latency_score = 1.0 / (1.0 + latency_ms / 100.0);
        return success_rate * 0.7 + latency_score * 0.3;
    }
};

/**
 * @brief Stateless sync state
 */
enum class StatelessSyncState {
    IDLE,               ///< Not syncing
    HEADERS_SYNC,       ///< Downloading headers
    PROOFS_SYNC,        ///< Downloading proofs
    BLOCKS_SYNC,        ///< Downloading blocks
    SYNCED              ///< Fully synced
};

/**
 * @brief Stateless sync statistics
 */
struct StatelessSyncStats {
    uint32_t current_height;           ///< Current sync height
    uint32_t target_height;            ///< Target chain tip height
    uint32_t proofs_requested;         ///< Total proofs requested
    uint32_t proofs_received;          ///< Total proofs received
    uint32_t proofs_validated;         ///< Total proofs validated successfully
    uint32_t proofs_failed;            ///< Total proof validation failures
    uint32_t blocks_applied;           ///< Total blocks applied to accumulator
    uint32_t root_continuity_errors;   ///< Number of root continuity breaks
    StatelessSyncState state;          ///< Current sync state
};

/**
 * @class StatelessNode
 * @brief Stateless full node that syncs using Utreexo proofs
 *
 * **Architecture:**
 * ```
 * StatelessNode
 *   ├── UtreexoForest* (accumulator management)
 *   ├── BridgePeer[] (bridge node pool)
 *   ├── ProofRequestQueue (pending proof requests)
 *   └── ValidationCache (verified proofs)
 * ```
 *
 * **Proof Validation Algorithm:**
 * ```
 * 1. Block hash matches proof message
 * 2. Root continuity: current_root == proof.root_before
 * 3. Extract targets from block inputs
 * 4. Verify Utreexo batch proof
 * 5. Apply block to accumulator (add/remove)
 * 6. Verify root_after matches
 * ```
 *
 * **Performance:**
 * - Typical block validation: ~5-10ms (proof verification)
 * - Parallel downloads: 8+ bridge nodes
 * - Expected sync time: ~90% of traditional full node
 * - Memory usage: ~100 MB (vs ~5 GB for full UTXO DB)
 *
 * **Thread Safety:**
 * - NOT thread-safe (caller must synchronize)
 * - Use external mutex for concurrent access
 */
class StatelessNode {
public:
    // ═════════════════════════════════════════════════════════════════════════
    // Constructor
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Construct a stateless node
     * @param utreexo_forest Pointer to Utreexo accumulator (must not be null)
     * @throws std::invalid_argument if utreexo_forest is null
     *
     * **Example:**
     * ```cpp
     * UtreexoForest forest;
     * StatelessNode node(&forest);
     * ```
     */
    explicit StatelessNode(consensus::UtreexoForest* utreexo_forest);

    /**
     * @brief Destructor
     */
    ~StatelessNode() = default;

    // Non-copyable, non-movable
    StatelessNode(const StatelessNode&) = delete;
    StatelessNode& operator=(const StatelessNode&) = delete;
    StatelessNode(StatelessNode&&) = delete;
    StatelessNode& operator=(StatelessNode&&) = delete;

    // ═════════════════════════════════════════════════════════════════════════
    // Peer Management
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Add a bridge node peer to the pool
     * @param peer Bridge peer information
     *
     * **Note:** Only peers with NODE_UTREEXO_BRIDGE service flag should be added.
     */
    void AddBridgePeer(const BridgePeer& peer);

    /**
     * @brief Remove a bridge node peer from the pool
     * @param peer_id Peer identifier to remove
     */
    void RemoveBridgePeer(uint64_t peer_id);

    /**
     * @brief Select best bridge nodes for proof requests
     * @param count Number of bridge nodes to select (default: 8)
     * @return Vector of selected bridge peers
     *
     * **Selection Criteria:**
     * 1. NODE_UTREEXO_BRIDGE service flag set
     * 2. Quality score (success rate + latency)
     * 3. Subnet diversity (avoid single subnet dependency)
     * 4. At least 8 nodes for DoS resistance
     *
     * **Quality Score:** success_rate * 0.7 + latency_score * 0.3
     */
    std::vector<BridgePeer> SelectBridgeNodes(size_t count = 8) const;

    /**
     * @brief Get all bridge peers in the pool
     * @return Vector of all bridge peers
     */
    std::vector<BridgePeer> GetBridgePeers() const;

    /**
     * @brief Clear all bridge peers
     */
    void ClearBridgePeers();

    // ═════════════════════════════════════════════════════════════════════════
    // Proof Request & Validation
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Request proofs for a batch of blocks
     * @param block_hashes Block hashes to request proofs for (max 16)
     * @param bridge_peer_callback Callback to send request to specific bridge peer
     * @return Number of proof requests sent
     *
     * **Load Balancing:**
     * Distributes requests across selected bridge nodes in round-robin fashion.
     *
     * **Example:**
     * ```cpp
     * auto callback = [&](uint64_t peer_id, const GetUtreexoProofMessage& request) {
     *     p2p_manager->SendMessage(peer_id, request);
     * };
     * node.RequestProofsForBlocks(block_hashes, callback);
     * ```
     *
     * **DoS Protection:** Max 16 blocks per batch (from design)
     */
    size_t RequestProofsForBlocks(
        const std::vector<uint256>& block_hashes,
        std::function<void(uint64_t peer_id, const GetUtreexoProofMessage&)> bridge_peer_callback
    );

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 8.2: Sync Loop Coordination
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Set block provider callback for fetching blocks during sync
     * @param provider Callback that fetches a block by hash
     *
     * **Phase 8.2 Requirement:** onProofResponse needs blocks to validate proofs.
     */
    void setBlockProvider(std::function<std::optional<Block>(const uint256&)> provider) {
        block_provider_ = provider;
    }

    /**
     * @brief Set proof request callback for requesting proofs from bridge nodes
     * @param requester Callback that sends proof request messages
     *
     * **Phase 8.2 Requirement:** Sync loop needs to request proofs autonomously.
     */
    void setProofRequester(std::function<void(const std::vector<uint256>&)> requester) {
        proof_requester_ = requester;
    }

    /**
     * @brief Set peer ban callback for banning misbehaving peers (Phase 8.4)
     * @param ban_callback Callback that bans a peer with reason
     *
     * **Phase 8.4 Requirement:** Ban peers that send invalid proofs (not just disconnect).
     */
    void setPeerBanCallback(std::function<void(const std::string& peer_id, const std::string& reason)> ban_callback) {
        peer_ban_callback_ = ban_callback;
    }

    /**
     * @brief Set peer disconnect callback for disconnecting timed-out peers (Phase 8.4)
     * @param disconnect_callback Callback that disconnects a peer
     *
     * **Phase 8.4 Peer Rotation:** Disconnect peers that repeatedly time out.
     */
    void setPeerDisconnectCallback(std::function<void(const std::string& peer_id)> disconnect_callback) {
        peer_disconnect_callback_ = disconnect_callback;
    }

    /**
     * @brief Request proofs for next batch of blocks in sync queue
     * @return Number of proof requests sent
     *
     * **Phase 8.2:** Core sync loop method.
     * - Takes next N blocks from pending headers
     * - Requests proofs via callback
     * - Tracks pending proof requests
     */
    size_t requestNextProofBatch();

    /**
     * @brief Check for timed-out proof requests and retry (Phase 8.3)
     * @return Number of timed-out proofs re-requested
     *
     * **Phase 8.3:** Timeout handling.
     * - Checks proof_request_times_ for requests older than proof_timeout_
     * - Re-requests timed-out proofs
     * - Should be called periodically (e.g., every 5 seconds)
     */
    size_t checkTimeoutsAndRetry();

    /**
     * @brief Advance sync tip after successful block validation
     * @param block_hash Block that was just validated
     * @param height Block height
     *
     * **Phase 8.2:** Moves sync cursor forward.
     * - Updates current_sync_height_
     * - Checks if more proofs needed
     * - Triggers next batch request if needed
     */
    void advanceSyncTip(const uint256& block_hash, uint32_t height);

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 8.2: P2P Response Handlers (Real Sync Loop)
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Handle incoming utxoproof response from bridge node (Phase 8.2)
     * @param peer Peer connection that sent the proof
     * @param proof_msg The proof message received
     * @return true if proof is valid and applied, false on validation failure
     *
     * **Phase 8.2 Behavior:**
     * - Fetches block via block_provider_ callback
     * - Validates proof cryptographically using ValidateUtreexoProof()
     * - Applies block to accumulator
     * - Advances sync tip via advanceSyncTip()
     * - On failure: Disconnects peer, logs specific error
     *
     * **No disconnects for missing infrastructure** - Phase 8.1 stubs removed.
     */
    bool onProofResponse(
        PeerConnection* peer,
        const UtreexoProofMessage& proof_msg
    );

    /**
     * @brief Handle proof response from a peer id/address without PeerConnection
     *
     * Used by auxiliary proof channels (for example gossip proofdata) where
     * we only have peer identity string and payload bytes.
     */
    bool onProofResponse(
        const std::string& peer_id,
        const UtreexoProofMessage& proof_msg
    );

    /**
     * @brief Track externally requested proof hash for anti-flood/timeout logic
     *
     * Call this when requesting proofs through non-primary channels so
     * incoming proof responses are treated as expected.
     */
    void TrackExternalProofRequest(const uint256& block_hash);

    /**
     * @brief Handle incoming utxohdrs response from bridge node (Phase 8.2)
     * @param peer Peer connection that sent the headers
     * @param headers_msg The headers message received
     * @return true if headers are valid, false on validation failure
     *
     * **Phase 8.2 Behavior:**
     * - Validates header chain continuity
     * - Stores headers in pending queue
     * - Schedules proof requests via requestNextProofBatch()
     * - On failure: Disconnects peer, logs error
     *
     * **Real sync scheduling** - Phase 8.1 stubs removed.
     */
    bool onHeadersResponse(
        PeerConnection* peer,
        const UtreexoHeadersMessage& headers_msg
    );

    /**
     * @brief Validate a Utreexo proof for a block
     * @param block The block to validate
     * @param proof_msg The proof message received from bridge node
     * @param peer_id The peer that provided the proof (for statistics)
     * @return true if proof is valid and root continuity maintained
     *
     * **Validation Steps:**
     * 1. Block hash matches proof_msg.block_hash
     * 2. Root continuity: current_root == proof_msg.accumulator_root_before
     * 3. Extract targets from block inputs (spent UTXOs)
     * 4. Verify Utreexo batch proof
     * 5. Apply block to accumulator (add new UTXOs, remove spent ones)
     * 6. Verify root_after: accumulator_root == proof_msg.accumulator_root_after
     *
     * **Security:**
     * - NO trust in bridge nodes required
     * - All proofs cryptographically verified
     * - Root continuity enforced at every block
     *
     * **Example:**
     * ```cpp
     * if (node.ValidateUtreexoProof(block, proof_msg, peer_id)) {
     *     // Block validated, accumulator updated
     *     ProcessBlock(block);
     * } else {
     *     // Proof invalid, ban peer
     *     BanPeer(peer_id);
     * }
     * ```
     */
    bool ValidateUtreexoProof(
        const Block& block,
        const UtreexoProofMessage& proof_msg,
        uint64_t peer_id
    );

    /**
     * @brief Validate block using transition proof
     *
     * **Algorithm:**
     * 1. Block hash must match proof message
     * 2. Commitment continuity: tp.roots_before commitment must match local_commitment_
     * 3. Core stateless verification via tp.verify() (batch proof + additions + intermediate roots)
     * 4. Post-state commitment must match block header commitment
     * 5. Advance local forest state from transition-proof contents
     * 6. Update local_commitment_ and local_num_leaves_
     *
     * @param block Block to validate
     * @param proof_msg Proof message metadata
     * @param tp Transition proof for stateless verification
     * @param peer_id Peer that sent the proof (for stats)
     * @return true if block + transition proof are valid
     */
    bool ValidateWithTransitionProof(
        const Block& block,
        const UtreexoProofMessage& proof_msg,
        const consensus::UtreexoTransitionProof& tp,
        uint64_t peer_id
    );

    /**
     * @brief Validate a transaction with per-input Utreexo inclusion proofs
     *
     * Verifies each non-coinbase input's inclusion in the accumulator WITHOUT
     * mutating the accumulator. Deletions happen at block time, not tx time.
     *
     * @param tx Transaction to validate
     * @param input_proofs Per-input (UtreexoProof, SpentOutputData), parallel to non-coinbase inputs
     * @param accumulator_root Root at proof generation time (freshness check)
     * @return true if all proofs verify against local commitment
     */
    bool ValidateUtreexoTx(
        const Transaction& tx,
        const std::vector<std::pair<consensus::UtreexoProof, consensus::SpentOutputData>>& input_proofs,
        const consensus::UtreexoHash& accumulator_root,
        uint32_t validation_height = 0
    );

    // ═════════════════════════════════════════════════════════════════════════
    // State Management
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get current accumulator root
     * @return Current Utreexo root commitment
     */
    consensus::UtreexoHash GetCurrentAccumulatorRoot() const;

    /**
     * @brief Get current sync height
     * @return Block height currently synced to
     */
    uint32_t GetSyncHeight() const;

    /**
     * @brief Set sync height (called after applying block)
     * @param height New sync height
     */
    void SetSyncHeight(uint32_t height);

    /**
     * @brief Re-synchronize cached stump tracking from the shared forest
     * @param height Optional sync height to apply alongside the forest state
     *
     * The forest is the canonical accumulator state in CSN mode. This repairs
     * local cached commitment/leaf counters after external checkpoint restore or
     * any other path that mutates the shared forest underneath StatelessNode.
     */
    void SyncToForestState(std::optional<uint32_t> height = std::nullopt);

    /**
     * @brief Get sync state
     * @return Current sync state
     */
    StatelessSyncState GetSyncState() const;

    /**
     * @brief Set sync state
     * @param state New sync state
     */
    void SetSyncState(StatelessSyncState state);

    /**
     * @brief Get sync statistics
     * @return Detailed sync statistics
     */
    StatelessSyncStats GetSyncStats() const;

    /**
     * @brief Reset sync statistics (for testing)
     */
    void ResetSyncStats();

    // ═════════════════════════════════════════════════════════════════════════
    // Internal Helpers (public for testing)
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Extract targets (leaf hashes) from block inputs
     * @param block Block to extract targets from
     * @param spent_outputs Spent output metadata (from proof message)
     * @return Vector of leaf hashes (targets for Utreexo proof)
     *
     * **Algorithm:**
     * For each transaction input (skip coinbase):
     *   1. Get spent output metadata (value, scriptPubKey)
     *   2. Compute leaf hash: Hash(txid || vout || value || scriptPubKey)
     *   3. Add to targets vector
     */
    std::vector<consensus::UtreexoHash> ExtractTargetsFromBlock(
        const Block& block,
        const std::vector<consensus::SpentOutputData>& spent_outputs
    ) const;

    /**
     * @brief Compute leaf hash for UTXO
     * @param txid Transaction ID
     * @param vout Output index
     * @param value Output value
     * @param script_pub_key Output script
     * @return Leaf hash
     *
     * **Format:** Hash(txid || vout || value || scriptPubKey)
     *
     * **Note:** Must match BridgeNode::ComputeLeafHash() algorithm exactly.
     */
    static consensus::UtreexoHash ComputeLeafHash(
        const uint256& txid,
        uint32_t vout,
        uint64_t value,
        const std::vector<uint8_t>& script_pub_key,
        uint32_t created_height = 0,
        bool is_coinbase = false
    );

    // ═════════════════════════════════════════════════════════════════════════
    // CSN Reorg Support (Checkpoint + Replay)
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Rewind forest and local state to a checkpoint at given height
     *
     * Restores the forest from a serialized snapshot and resets all
     * tracking state (commitment, num_leaves, sync height).
     * Used during STATELESS reorg to return to the fork point.
     */
    void RewindToCheckpoint(uint32_t height, const consensus::UtreexoForest& restored_forest);

    /**
     * @brief Replay a block through the forest using pre-stored spend targets
     *
     * Like ApplyBlockToAccumulator but takes explicit spend target hashes
     * instead of reading from proof_data. Used during reorg connect phase
     * where transition proofs are not available.
     *
     * Invariant enforcement:
     * - Replayed AFTER-state root must equal block.header.utreexo_root
     * - Forest updates are transactional (no partial mutation on failure)
     *
     * @param block The block to replay
     * @param spend_targets Leaf hashes of UTXOs spent in this block
     * @param spent_outputs Optional per-input metadata used for consensus rule checks
     * @return true if forest was successfully advanced
     */
    bool ReplayBlock(
        const Block& block,
        uint32_t block_height,
        const std::vector<consensus::UtreexoHash>& spend_targets,
        const std::vector<consensus::SpentOutputData>* spent_outputs = nullptr);

    /**
     * @brief Get raw pointer to the underlying forest
     */
    consensus::UtreexoForest* GetForest() const { return utreexo_forest_; }

    /**
     * @brief Get the stump accumulator (roots-only, ~2KB)
     */
    const consensus::UtreexoStump& GetStump() const { return local_stump_; }

private:
    // ═════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═════════════════════════════════════════════════════════════════════════

    /// Utreexo accumulator (NOT owned by this class)
    consensus::UtreexoForest* utreexo_forest_;

    /// Stump accumulator — canonical CSN state tracker.
    /// Uses pure root math (no findLeafPosition, no internal node map).
    consensus::UtreexoStump local_stump_;

    /// Cached commitment/leaf-count from stump for fast continuity checks.
    consensus::UtreexoHash local_commitment_;
    uint64_t local_num_leaves_ = 0;

    /// Bridge node peer pool
    std::map<uint64_t, BridgePeer> bridge_peers_;

    /// Current sync height
    uint32_t current_sync_height_;

    /// Current sync state
    StatelessSyncState sync_state_;

    /// Sync statistics
    StatelessSyncStats sync_stats_;

    /// Round-robin index for load balancing
    mutable size_t next_bridge_index_;

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 8.2: Sync Loop State
    // ═════════════════════════════════════════════════════════════════════════

    /// Pending headers (headers we have but no proofs yet)
    std::vector<BlockHeader> pending_headers_;

    /// Pending proof requests (block hashes we've requested proofs for)
    std::set<uint256> pending_proofs_;

    /// Block provider callback (fetch block by hash)
    std::function<std::optional<Block>(const uint256&)> block_provider_;

    /// Proof requester callback (request proofs for block hashes)
    std::function<void(const std::vector<uint256>&)> proof_requester_;

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 8.3: Atomic Proof ↔ Block Correlation
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Pending block and proof pair for atomic validation
     *
     * **Phase 8.3 Atomicity Guarantee:**
     * Validation and accumulator application ONLY happen when both block and proof are present.
     * This enforces atomicity and handles out-of-order arrival.
     */
    struct PendingBlockProof {
        uint256 block_hash;
        Block block;
        UtreexoProofMessage proof;
        bool has_block = false;
        bool has_proof = false;
        std::string peer_id;  // Peer that provided the proof (for ban logic)
    };

    /// Pending block/proof pairs (atomicity enforcement)
    std::unordered_map<uint256, PendingBlockProof> pending_;

    /**
     * @brief Apply pending block/proof pair atomically (Phase 8.3)
     * @param block_hash Block hash to apply
     * @return true if validation and application succeeded, false otherwise
     *
     * **Phase 8.3 Single Atomic Path:**
     * This is the ONLY function that calls ValidateUtreexoProof and ApplyBlockToAccumulator.
     * Enforces atomicity: both block and proof must be present before validation.
     *
     * **Verification:**
     * grep -r 'ValidateUtreexoProof' src/ | grep -v applyPending  # MUST be empty
     * grep -r 'ApplyBlockToAccumulator' src/ | grep -v applyPending  # MUST be empty
     */
    bool applyPending(const uint256& block_hash);

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 8.3/8.4: Error Handling & Retry Logic
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Pending proof request with timeout tracking
     *
     * **Phase 8.4 Minimal Peer Rotation:**
     * Timeout ≠ malicious. On timeout: disconnect (not ban) and rotate to next peer.
     */
    struct PendingProofRequest {
        uint256 block_hash;
        std::chrono::steady_clock::time_point request_time;
        std::string current_peer_id;  // Peer we last requested from
    };

    /// Pending proof requests with timeout tracking
    std::map<uint256, PendingProofRequest> proof_requests_;

    /// Peers already tried for each block (to avoid immediate retry)
    std::unordered_map<uint256, std::set<std::string>> tried_peers_;

    /// Proof request timeout (15 seconds - conservative)
    std::chrono::seconds proof_timeout_{15};

    /// Peer ban callback (ban peer for invalid proof only, NOT timeout)
    std::function<void(const std::string& peer_id, const std::string& reason)> peer_ban_callback_;

    /// Peer disconnect callback (disconnect peer on timeout)
    std::function<void(const std::string& peer_id)> peer_disconnect_callback_;

    /// DoS hardening: unsolicited proof flood tracking (per-peer counters)
    std::unordered_map<std::string, uint32_t> unsolicited_proof_counts_;
    std::unordered_map<std::string, uint32_t> stale_proof_counts_;
    static constexpr uint32_t MAX_UNSOLICITED_PROOFS_PER_PEER = 8;
    static constexpr uint32_t MAX_STALE_PROOFS_PER_PEER = 16;
};

} // namespace network
} // namespace dinero
