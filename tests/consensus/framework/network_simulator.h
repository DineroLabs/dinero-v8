#pragma once

#include "consensus_types.h"
#include "consensus_node.h"
#include <memory>
#include <map>
#include <vector>
#include <functional>
#include <optional>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DeliveredMessage - Record of message delivery
 */
struct DeliveredMessage {
    NodeID from;
    NodeID to;
    ConsensusEventType type;
    std::string payload;
    uint64_t send_time;
    uint64_t delivery_time;
};

/**
 * NetworkSimulator - Message routing for multi-node consensus simulation
 *
 * Extends Ring 3's MockSocket pattern to multi-node scenarios with:
 * - Deterministic message delivery scheduling
 * - Network latency simulation
 * - Packet loss simulation
 * - Network partitions (routing filters)
 * - Message ordering (TCP within connection, async between connections)
 *
 * Design:
 * - Each node pair has a simulated connection
 * - Messages are queued with delivery timestamp = send_time + latency
 * - Partitions block message delivery between partition groups
 * - DeterministicScheduler (Ring 3) controls event ordering
 */
class NetworkSimulator {
public:
    /**
     * Create network simulator
     *
     * @param topology Network connectivity graph
     * @param rng_seed Deterministic RNG seed
     */
    NetworkSimulator(const NetworkTopology& topology, uint64_t rng_seed);

    ~NetworkSimulator();

    // ========================================================================
    // Network Configuration
    // ========================================================================

    /**
     * Set base latency for all connections (milliseconds)
     */
    void setGlobalLatency(uint64_t latency_ms);

    /**
     * Set latency for specific connection (milliseconds)
     */
    void setConnectionLatency(const NodeID& from, const NodeID& to, uint64_t latency_ms);

    /**
     * Set packet loss rate for all connections (0.0 to 1.0)
     */
    void setGlobalPacketLoss(double loss_rate);

    /**
     * Set packet loss rate for specific connection (0.0 to 1.0)
     */
    void setConnectionPacketLoss(const NodeID& from, const NodeID& to, double loss_rate);

    // ========================================================================
    // Partition Control
    // ========================================================================

    /**
     * Partition network into groups
     *
     * Nodes within same group can communicate, nodes in different groups cannot.
     *
     * @param groups List of node groups (e.g., {{"alice", "bob"}, {"carol"}})
     */
    void partitionNetwork(const std::vector<std::vector<NodeID>>& groups);

    /**
     * Heal all partitions (restore full connectivity)
     */
    void healPartitions();

    /**
     * Check if two nodes can communicate (not partitioned)
     */
    bool canCommunicate(const NodeID& from, const NodeID& to) const;

    // ========================================================================
    // Message Routing
    // ========================================================================

    /**
     * Send message from one node to another
     *
     * Message is queued for delivery at (current_time + latency).
     * Subject to packet loss and partition filtering.
     *
     * @param from Sender node ID
     * @param to Receiver node ID
     * @param message_type Type of message
     * @param payload Message content (block hash, tx id, etc.)
     * @param current_time Current simulation time
     * @return true if message queued, false if dropped (packet loss or partition)
     */
    bool sendMessage(
        const NodeID& from,
        const NodeID& to,
        ConsensusEventType message_type,
        const std::string& payload,
        uint64_t current_time
    );

    /**
     * Deliver all messages scheduled for delivery at or before current_time
     *
     * Called by ConsensusSimulator each tick.
     *
     * @param current_time Current simulation time
     * @param nodes Map of node_id → ConsensusNode*
     * @return List of delivered messages
     */
    std::vector<DeliveredMessage> deliverMessages(
        uint64_t current_time,
        std::map<NodeID, ConsensusNode*>& nodes
    );

    /**
     * Get pending message count
     */
    size_t getPendingMessageCount() const;

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get total messages sent
     */
    uint64_t getTotalMessagesSent() const { return total_messages_sent_; }

    /**
     * Get total messages delivered
     */
    uint64_t getTotalMessagesDelivered() const { return total_messages_delivered_; }

    /**
     * Get total messages dropped (packet loss)
     */
    uint64_t getTotalMessagesDropped() const { return total_messages_dropped_; }

    /**
     * Get total messages blocked (partition)
     */
    uint64_t getTotalMessagesBlocked() const { return total_messages_blocked_; }

private:
    // Network topology
    NetworkTopology topology_;

    // RNG for packet loss
    uint64_t rng_seed_;
    uint64_t rng_state_;

    // Network parameters
    uint64_t global_latency_ms_;
    double global_packet_loss_;

    // Per-connection overrides
    struct ConnectionKey {
        NodeID from;
        NodeID to;

        bool operator<(const ConnectionKey& other) const {
            if (from != other.from) return from < other.from;
            return to < other.to;
        }
    };
    std::map<ConnectionKey, uint64_t> connection_latency_;
    std::map<ConnectionKey, double> connection_packet_loss_;

    // Partition state
    bool is_partitioned_;
    std::vector<std::vector<NodeID>> partition_groups_;

    // Message queue
    struct PendingMessage {
        NodeID from;
        NodeID to;
        ConsensusEventType type;
        std::string payload;
        uint64_t delivery_time;
    };
    std::vector<PendingMessage> message_queue_;

    // Statistics
    uint64_t total_messages_sent_;
    uint64_t total_messages_delivered_;
    uint64_t total_messages_dropped_;
    uint64_t total_messages_blocked_;

    // Helper: Get latency for connection
    uint64_t getLatency(const NodeID& from, const NodeID& to) const;

    // Helper: Get packet loss rate for connection
    double getPacketLoss(const NodeID& from, const NodeID& to) const;

    // Helper: Roll for packet loss (deterministic RNG)
    bool shouldDropPacket(double loss_rate);

    // Helper: Find partition group for node
    std::optional<int> getPartitionGroup(const NodeID& node_id) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
