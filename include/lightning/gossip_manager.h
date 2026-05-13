#pragma once

#include "lightning/gossip_types.h"
#include "lightning/lightning_types.h"
#include "din_json.h"
#include "result.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <functional>

// Forward declaration
struct DaemonContext;

namespace dinero {
namespace lightning {

// Forward declarations
class ChannelManager;

/**
 * @class GossipManager
 * @brief Manages Lightning Network gossip protocol (BOLT #7)
 *
 * Phase 9: Dynamic Channel Update Processing
 *
 * Responsibilities:
 * - Store and validate channel announcements
 * - Process channel updates with timestamp filtering
 * - Maintain network graph topology
 * - Propagate gossip to peers
 * - Score channels based on local observations
 * - Feed real-time updates to PaymentRouter
 *
 * BOLT #7 Features:
 * - Channel announcements (proof of on-chain funding)
 * - Node announcements (network addresses, features)
 * - Channel updates (routing policies, fees, capacity)
 * - Gossip queries (sync protocol)
 * - Timestamp filters (bandwidth optimization)
 *
 * Thread Safety: All public methods are thread-safe
 */
class GossipManager {
public:
    /**
     * @brief Construct GossipManager
     * @param ctx Reference to DaemonContext
     * @param channel_mgr Reference to ChannelManager for local channels
     */
    GossipManager(
        DaemonContext& ctx,
        ChannelManager& channel_mgr
    );
    ~GossipManager();

    // ═══════════════════════════════════════════════════════════════════════════
    // Channel Update Processing (Phase 9.1)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Process incoming channel update
     *
     * Validates signature, checks timestamp, updates graph, propagates to peers.
     *
     * @param update Channel update message
     * @param from_peer Peer that sent this update (empty if local)
     * @return Result<void> Success or validation error
     */
    Result<void> processChannelUpdate(
        const ChannelUpdate& update,
        const std::string& from_peer = ""
    );

    /**
     * @brief Process channel announcement
     *
     * @param announcement Channel announcement message
     * @param from_peer Peer that sent this announcement
     * @return Result<void> Success or validation error
     */
    Result<void> processChannelAnnouncement(
        const ChannelAnnouncement& announcement,
        const std::string& from_peer = ""
    );

    /**
     * @brief Process node announcement
     *
     * @param announcement Node announcement message
     * @param from_peer Peer that sent this announcement
     * @return Result<void> Success or validation error
     */
    Result<void> processNodeAnnouncement(
        const NodeAnnouncement& announcement,
        const std::string& from_peer = ""
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Timestamp Filtering (Phase 9.2)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Set gossip timestamp filter for a peer
     *
     * Tells peer to only send updates after first_timestamp.
     *
     * @param peer_id Peer to filter
     * @param first_timestamp Unix timestamp to start from
     * @param timestamp_range How far into future to accept (seconds)
     * @return Result<void> Success or error
     */
    Result<void> setTimestampFilter(
        const std::string& peer_id,
        uint32_t first_timestamp,
        uint32_t timestamp_range = 0xFFFFFFFF  // Default: no upper bound
    );

    /**
     * @brief Get updates since timestamp
     *
     * Returns all channel/node updates with timestamp >= first_timestamp.
     *
     * @param first_timestamp Unix timestamp
     * @return std::vector<ChannelUpdate> Updates since timestamp
     */
    std::vector<ChannelUpdate> getUpdatesSince(uint32_t first_timestamp) const;

    /**
     * @brief Check if update passes timestamp filter
     *
     * @param update Update to check
     * @param peer_id Peer's filter to check against
     * @return bool True if update should be sent to peer
     */
    bool passesTimestampFilter(
        const ChannelUpdate& update,
        const std::string& peer_id
    ) const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Propagation Logic (Phase 9.3)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Propagate update to all peers except origin
     *
     * @param update Update to propagate
     * @param except_peer Don't send to this peer (usually the origin)
     * @return size_t Number of peers propagated to
     */
    size_t propagateUpdate(
        const ChannelUpdate& update,
        const std::string& except_peer = ""
    );

    /**
     * @brief Register propagation callback
     *
     * Called when updates need to be sent to peers.
     *
     * @param callback Function(peer_id, serialized_message)
     */
    using PropagationCallback = std::function<void(const std::string&, const std::vector<uint8_t>&)>;
    void registerPropagationCallback(PropagationCallback callback);

    // ═══════════════════════════════════════════════════════════════════════════
    // Local Channel Scoring (Phase 9.4)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @struct ChannelScore
     * @brief Local scoring of channel reliability and performance
     */
    struct ChannelScore {
        std::string channel_id;
        uint64_t short_channel_id;

        // Success metrics
        uint32_t successful_forwards = 0;
        uint32_t failed_forwards = 0;
        uint64_t total_forwarded_muna = 0;

        // Timing metrics
        uint64_t avg_forward_time_ms = 0;

        // Reliability score (0-100)
        double reliability_score = 50.0;

        // Last interaction
        uint64_t last_success_at = 0;
        uint64_t last_failure_at = 0;

        // Computed score (0-1000, higher = better)
        uint32_t computed_score() const {
            double base_score = reliability_score * 10.0;  // 0-1000

            // Phase 8.5: Penalize recent failures (timestamp from ITimeOracle)
            // uint64_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            uint64_t now = 0;  // TODO: Pass ITimeOracle to computed_score()
            if (last_failure_at > last_success_at && (now - last_failure_at) < 3600) {
                base_score *= 0.5;  // 50% penalty for failures in last hour
            }

            // Bonus for high success rate
            if (successful_forwards + failed_forwards > 10) {
                double success_rate = static_cast<double>(successful_forwards) /
                                     (successful_forwards + failed_forwards);
                base_score *= (0.5 + success_rate * 0.5);  // 50-100% multiplier
            }

            return static_cast<uint32_t>(std::min(base_score, 1000.0));
        }
    };

    /**
     * @brief Update channel score based on forward result
     *
     * @param channel_id Channel that forwarded (or failed)
     * @param success True if forward succeeded
     * @param amount_muna Amount forwarded
     * @param latency_ms Time taken
     */
    void updateChannelScore(
        const std::string& channel_id,
        bool success,
        uint64_t amount_muna = 0,
        uint64_t latency_ms = 0
    );

    /**
     * @brief Get channel score
     *
     * @param channel_id Channel to query
     * @return std::optional<ChannelScore> Score if channel is tracked
     */
    std::optional<ChannelScore> getChannelScore(const std::string& channel_id) const;

    /**
     * @brief Get all channel scores
     *
     * @return std::vector<ChannelScore> All tracked channel scores
     */
    std::vector<ChannelScore> getAllChannelScores() const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Network Graph Access (Phase 9.5 - PaymentRouter Integration)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get channel update for routing
     *
     * @param short_channel_id Channel to query
     * @param direction 0 = node_1→node_2, 1 = node_2→node_1
     * @return std::optional<ChannelUpdate> Update if exists
     */
    std::optional<ChannelUpdate> getChannelUpdate(
        uint64_t short_channel_id,
        uint8_t direction
    ) const;

    /**
     * @brief Get all active channels
     *
     * @return std::vector<ChannelEdge> All enabled channels
     */
    std::vector<ChannelEdge> getActiveChannels() const;

    /**
     * @brief Register update callback for PaymentRouter
     *
     * Called whenever channel updates arrive.
     *
     * @param callback Function(channel_id, update)
     */
    using UpdateCallback = std::function<void(uint64_t, const ChannelUpdate&)>;
    void registerUpdateCallback(UpdateCallback callback);

    // ═══════════════════════════════════════════════════════════════════════════
    // Gossip Sync Protocol
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Request gossip sync from peer
     *
     * @param peer_id Peer to sync from
     * @param first_block Start block height
     * @param num_blocks Number of blocks to query
     * @return Result<void> Success or error
     */
    Result<void> requestGossipSync(
        const std::string& peer_id,
        uint32_t first_block,
        uint32_t num_blocks
    );

    /**
     * @brief Get graph statistics
     *
     * @return din::Json Stats (nodes, channels, updates, etc.)
     */
    din::Json getGraphStats() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;
    ChannelManager& m_channel_mgr;

    // Channel updates (short_channel_id|direction → ChannelUpdate)
    mutable std::mutex m_updates_mutex;
    std::map<std::pair<uint64_t, uint8_t>, ChannelUpdate> m_channel_updates;

    // Channel announcements (short_channel_id → ChannelAnnouncement)
    mutable std::mutex m_announcements_mutex;
    std::map<uint64_t, ChannelAnnouncement> m_channel_announcements;

    // Node announcements (node_id → NodeAnnouncement)
    mutable std::mutex m_nodes_mutex;
    std::map<NodeID, NodeAnnouncement> m_node_announcements;

    // Timestamp filters per peer (peer_id → filter)
    mutable std::mutex m_filters_mutex;
    std::map<std::string, GossipTimestampFilter> m_timestamp_filters;

    // Channel scores (channel_id → score)
    mutable std::mutex m_scores_mutex;
    std::map<std::string, ChannelScore> m_channel_scores;

    // Propagation tracking (update_hash → set of peers we've sent to)
    mutable std::mutex m_propagation_mutex;
    std::map<std::string, std::set<std::string>> m_propagated_to;

    // Callbacks
    std::vector<PropagationCallback> m_propagation_callbacks;
    std::vector<UpdateCallback> m_update_callbacks;
    mutable std::mutex m_callbacks_mutex;

    // Statistics
    struct Stats {
        uint64_t channel_updates_received = 0;
        uint64_t channel_updates_sent = 0;
        uint64_t announcements_received = 0;
        uint64_t announcements_sent = 0;
        uint64_t updates_rejected = 0;
        uint64_t updates_stale = 0;
    };
    mutable Stats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Validation
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Validate channel update
     *
     * Checks signature, timestamp, and channel existence.
     *
     * @param update Update to validate
     * @return bool True if valid
     */
    bool validateChannelUpdate(const ChannelUpdate& update) const;

    /**
     * @brief Check if update is stale
     *
     * @param update Update to check
     * @return bool True if timestamp is older than stored version
     */
    bool isStaleUpdate(const ChannelUpdate& update) const;

    /**
     * @brief Get update hash for deduplication
     *
     * @param update Update to hash
     * @return std::string Hash of update
     */
    std::string getUpdateHash(const ChannelUpdate& update) const;
};

} // namespace lightning
} // namespace dinero
