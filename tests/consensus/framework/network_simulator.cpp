#include "network_simulator.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

NetworkSimulator::NetworkSimulator(const NetworkTopology& topology, uint64_t rng_seed)
    : topology_(topology)
    , rng_seed_(rng_seed)
    , rng_state_(rng_seed)
    , global_latency_ms_(10)  // Default 10ms
    , global_packet_loss_(0.0)  // Default no loss
    , is_partitioned_(false)
    , total_messages_sent_(0)
    , total_messages_delivered_(0)
    , total_messages_dropped_(0)
    , total_messages_blocked_(0)
{
}

NetworkSimulator::~NetworkSimulator() {
}

// ============================================================================
// Network Configuration
// ============================================================================

void NetworkSimulator::setGlobalLatency(uint64_t latency_ms) {
    global_latency_ms_ = latency_ms;
}

void NetworkSimulator::setConnectionLatency(const NodeID& from, const NodeID& to, uint64_t latency_ms) {
    ConnectionKey key{from, to};
    connection_latency_[key] = latency_ms;
}

void NetworkSimulator::setGlobalPacketLoss(double loss_rate) {
    global_packet_loss_ = loss_rate;
}

void NetworkSimulator::setConnectionPacketLoss(const NodeID& from, const NodeID& to, double loss_rate) {
    ConnectionKey key{from, to};
    connection_packet_loss_[key] = loss_rate;
}

// ============================================================================
// Partition Control
// ============================================================================

void NetworkSimulator::partitionNetwork(const std::vector<std::vector<NodeID>>& groups) {
    is_partitioned_ = true;
    partition_groups_ = groups;
}

void NetworkSimulator::healPartitions() {
    is_partitioned_ = false;
    partition_groups_.clear();
}

bool NetworkSimulator::canCommunicate(const NodeID& from, const NodeID& to) const {
    if (!is_partitioned_) {
        return true;  // No partitions
    }

    // Check if both nodes are in the same partition group
    auto from_group = getPartitionGroup(from);
    auto to_group = getPartitionGroup(to);

    if (!from_group || !to_group) {
        return true;  // Node not in any partition group
    }

    return from_group == to_group;  // Same group = can communicate
}

// ============================================================================
// Message Routing
// ============================================================================

bool NetworkSimulator::sendMessage(
    const NodeID& from,
    const NodeID& to,
    ConsensusEventType message_type,
    const std::string& payload,
    uint64_t current_time
) {
    total_messages_sent_++;

    // Check partition
    if (!canCommunicate(from, to)) {
        total_messages_blocked_++;
        return false;
    }

    // Check packet loss
    double loss_rate = getPacketLoss(from, to);
    if (shouldDropPacket(loss_rate)) {
        total_messages_dropped_++;
        return false;
    }

    // Calculate delivery time
    uint64_t latency = getLatency(from, to);
    uint64_t delivery_time = current_time + latency;

    // Enqueue message
    PendingMessage msg;
    msg.from = from;
    msg.to = to;
    msg.type = message_type;
    msg.payload = payload;
    msg.delivery_time = delivery_time;
    message_queue_.push_back(msg);

    return true;
}

std::vector<DeliveredMessage> NetworkSimulator::deliverMessages(
    uint64_t current_time,
    std::map<NodeID, ConsensusNode*>& nodes
) {
    std::vector<DeliveredMessage> delivered;
    std::vector<PendingMessage> remaining;

    for (const auto& msg : message_queue_) {
        if (msg.delivery_time <= current_time) {
            // Deliver message
            auto it = nodes.find(msg.to);
            if (it != nodes.end()) {
                ConsensusNode* node = it->second;
                node->enqueueMessage(msg.from, msg.type, msg.payload, current_time);

                // Record delivery
                DeliveredMessage delivered_msg;
                delivered_msg.from = msg.from;
                delivered_msg.to = msg.to;
                delivered_msg.type = msg.type;
                delivered_msg.payload = msg.payload;
                delivered_msg.send_time = msg.delivery_time - getLatency(msg.from, msg.to);
                delivered_msg.delivery_time = msg.delivery_time;
                delivered.push_back(delivered_msg);

                total_messages_delivered_++;
            }
        } else {
            // Message not ready yet
            remaining.push_back(msg);
        }
    }

    message_queue_ = remaining;
    return delivered;
}

size_t NetworkSimulator::getPendingMessageCount() const {
    return message_queue_.size();
}

// ============================================================================
// Helpers
// ============================================================================

uint64_t NetworkSimulator::getLatency(const NodeID& from, const NodeID& to) const {
    ConnectionKey key{from, to};
    auto it = connection_latency_.find(key);
    if (it != connection_latency_.end()) {
        return it->second;
    }
    return global_latency_ms_;
}

double NetworkSimulator::getPacketLoss(const NodeID& from, const NodeID& to) const {
    ConnectionKey key{from, to};
    auto it = connection_packet_loss_.find(key);
    if (it != connection_packet_loss_.end()) {
        return it->second;
    }
    return global_packet_loss_;
}

bool NetworkSimulator::shouldDropPacket(double loss_rate) {
    if (loss_rate <= 0.0) {
        return false;
    }
    if (loss_rate >= 1.0) {
        return true;
    }

    // Simple LCG (linear congruential generator) for deterministic RNG
    rng_state_ = (rng_state_ * 1103515245ULL + 12345ULL) & 0x7fffffffULL;
    double random_value = static_cast<double>(rng_state_) / 0x7fffffffULL;

    return random_value < loss_rate;
}

std::optional<int> NetworkSimulator::getPartitionGroup(const NodeID& node_id) const {
    for (size_t i = 0; i < partition_groups_.size(); i++) {
        const auto& group = partition_groups_[i];
        if (std::find(group.begin(), group.end(), node_id) != group.end()) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

} // namespace test
} // namespace consensus
} // namespace dinero
