#pragma once

#include "gossip_types.h"
#include "lightning_types.h"
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>

namespace dinero {
namespace lightning {

// Network graph for routing and topology
class NetworkGraph {
public:
    NetworkGraph();
    ~NetworkGraph();

    // Channel management
    Result<void> add_channel_announcement(const ChannelAnnouncement& announcement);
    Result<void> add_channel_update(const ChannelUpdate& update);
    Result<void> remove_channel(const ShortChannelID& scid);

    // Node management
    Result<void> add_node_announcement(const NodeAnnouncement& announcement);
    Result<void> remove_node(const NodeID& node_id);

    // Queries
    std::optional<ChannelEdge> get_channel(const ShortChannelID& scid, bool direction) const;
    std::optional<GraphNode> get_node(const NodeID& node_id) const;
    std::vector<ShortChannelID> get_channels_for_node(const NodeID& node_id) const;
    std::vector<NodeID> get_all_nodes() const;
    std::vector<ShortChannelID> get_all_channels() const;

    // Route finding
    std::optional<Route> find_route(
        const NodeID& source,
        const NodeID& target,
        uint64_t amount_muna,
        uint32_t max_cltv
    ) const;

    // Multi-path routing
    std::vector<Route> find_multi_path_routes(
        const NodeID& source,
        const NodeID& target,
        uint64_t amount_muna,
        size_t max_paths
    ) const;

    // Channel range queries
    std::vector<ShortChannelID> get_channels_in_range(
        uint32_t first_block,
        uint32_t num_blocks
    ) const;

    // Statistics
    size_t num_nodes() const;
    size_t num_channels() const;
    uint64_t total_capacity() const;

    // Pruning
    size_t prune_stale_channels(uint32_t current_timestamp, uint32_t max_age_seconds);
    size_t prune_stale_nodes(uint32_t current_timestamp, uint32_t max_age_seconds);

    // Validation
    bool validate_channel_announcement(const ChannelAnnouncement& announcement) const;
    bool validate_channel_update(const ChannelUpdate& update) const;
    bool validate_node_announcement(const NodeAnnouncement& announcement) const;

    // Serialization (for persistence)
    std::vector<uint8_t> serialize() const;
    static std::optional<NetworkGraph> deserialize(const std::vector<uint8_t>& data);

private:
    struct ChannelInfo {
        ChannelAnnouncement announcement;
        std::optional<ChannelUpdate> update_1;  // Direction 0
        std::optional<ChannelUpdate> update_2;  // Direction 1
        uint64_t capacity_sat;
        uint32_t last_update;
    };

    // Graph storage
    std::map<ShortChannelID, ChannelInfo> channels_;
    std::map<NodeID, GraphNode> nodes_;

    // Indexes for fast lookup
    std::map<NodeID, std::set<ShortChannelID>> node_to_channels_;
    std::map<uint32_t, std::set<ShortChannelID>> block_to_channels_;

    mutable std::mutex mutex_;

    // Helper methods
    std::vector<ChannelEdge> get_outgoing_edges(const NodeID& node) const;
    uint64_t estimate_capacity(const ShortChannelID& scid) const;
    std::optional<ChannelEdge> get_channel(const ShortChannelID& scid, bool direction) const;
    std::vector<ShortChannelID> get_channels_for_node(const NodeID& node_id) const;

    // Helper: Find route while avoiding specific edges (for multi-path)
    std::optional<Route> find_route_avoiding(
        const NodeID& source,
        const NodeID& target,
        uint64_t amount_muna,
        const std::set<std::pair<NodeID, NodeID>>& avoid_edges
    ) const;
};

// Gossip store for persisting messages
class GossipStore {
public:
    explicit GossipStore(const std::string& db_path);
    ~GossipStore();

    // Store gossip messages
    Result<void> store_channel_announcement(const ChannelAnnouncement& announcement);
    Result<void> store_channel_update(const ChannelUpdate& update);
    Result<void> store_node_announcement(const NodeAnnouncement& announcement);

    // Retrieve messages
    std::optional<ChannelAnnouncement> get_channel_announcement(const ShortChannelID& scid);
    std::optional<ChannelUpdate> get_channel_update(const ShortChannelID& scid, bool direction);
    std::optional<NodeAnnouncement> get_node_announcement(const NodeID& node_id);

    // Query by timestamp
    std::vector<ChannelAnnouncement> get_channel_announcements_since(uint32_t timestamp);
    std::vector<ChannelUpdate> get_channel_updates_since(uint32_t timestamp);
    std::vector<NodeAnnouncement> get_node_announcements_since(uint32_t timestamp);

    // Prune old messages
    size_t prune_older_than(uint32_t timestamp);

    // Statistics
    size_t get_message_count() const;
    uint64_t get_storage_size() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lightning
} // namespace dinero
