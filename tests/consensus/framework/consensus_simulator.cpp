#include "consensus_simulator.h"

namespace dinero {
namespace consensus {
namespace test {

ConsensusSimulator::ConsensusSimulator(
    const NetworkTopology& topology,
    const ChainParams& params,
    uint64_t rng_seed,
    const std::string& scenario_name
)
    : topology_(topology)
    , params_(params)
    , rng_seed_(rng_seed)
    , scenario_name_(scenario_name)
    , running_(false)
    , current_time_(0)
    , action_sequence_(0)
    , snapshot_interval_ms_(1000)  // Default: snapshot every 1 second
    , last_snapshot_time_(0)
{
    // Create network simulator
    network_ = std::make_unique<NetworkSimulator>(topology, rng_seed);

    // Create all nodes from topology
    for (const auto& node_id : topology.nodes) {
        auto node = std::make_unique<ConsensusNode>(node_id, params, rng_seed);
        nodes_[node_id] = std::move(node);
    }
}

ConsensusSimulator::~ConsensusSimulator() {
    if (running_) {
        stop();
    }
}

// ============================================================================
// Simulation Lifecycle
// ============================================================================

void ConsensusSimulator::start() {
    if (running_) {
        return;
    }

    running_ = true;
    current_time_ = 0;
    action_sequence_ = 0;

    // Start all nodes
    for (auto& [node_id, node] : nodes_) {
        node->start();
        recordAction(ConsensusActionType::NODE_START, node_id);
    }

    // Establish connections per topology
    establishConnections();

    // Capture initial state
    captureSnapshots();
}

void ConsensusSimulator::stop() {
    if (!running_) {
        return;
    }

    // Stop all nodes
    for (auto& [node_id, node] : nodes_) {
        node->stop();
        recordAction(ConsensusActionType::NODE_STOP, node_id);
    }

    // Collect final events
    collectEvents();

    running_ = false;
}

void ConsensusSimulator::tick(uint64_t delta_ms) {
    if (!running_) {
        return;
    }

    // Advance time
    current_time_ += delta_ms;
    recordAction(ConsensusActionType::ADVANCE_TIME);

    // Deliver pending messages
    std::map<NodeID, ConsensusNode*> node_ptrs;
    for (auto& [node_id, node] : nodes_) {
        node_ptrs[node_id] = node.get();
    }
    network_->deliverMessages(current_time_, node_ptrs);

    // Process messages at each node
    for (auto& [node_id, node] : nodes_) {
        node->processMessages(current_time_);
    }

    // Capture snapshots if interval elapsed
    if (snapshot_interval_ms_ > 0 &&
        current_time_ - last_snapshot_time_ >= snapshot_interval_ms_) {
        captureSnapshots();
        last_snapshot_time_ = current_time_;
    }
}

void ConsensusSimulator::run(uint64_t duration_ms, uint64_t tick_interval_ms) {
    uint64_t end_time = current_time_ + duration_ms;
    while (current_time_ < end_time) {
        tick(tick_interval_ms);
    }
}

// ============================================================================
// Node Control
// ============================================================================

void ConsensusSimulator::nodeStart(const NodeID& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    it->second->start();
    recordAction(ConsensusActionType::NODE_START, node_id);
}

void ConsensusSimulator::nodeStop(const NodeID& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    it->second->stop();
    recordAction(ConsensusActionType::NODE_STOP, node_id);
}

void ConsensusSimulator::nodeStartMining(const NodeID& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    it->second->startMining();
    recordAction(ConsensusActionType::START_MINING, node_id);
}

void ConsensusSimulator::nodeStopMining(const NodeID& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    it->second->stopMining();
    recordAction(ConsensusActionType::STOP_MINING, node_id);
}

ConsensusNode* ConsensusSimulator::getNode(const NodeID& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return nullptr;
    }
    return it->second.get();
}

// ============================================================================
// Network Control
// ============================================================================

void ConsensusSimulator::partitionNetwork(const std::vector<std::vector<NodeID>>& groups) {
    network_->partitionNetwork(groups);
    recordAction(ConsensusActionType::PARTITION_NETWORK);
}

void ConsensusSimulator::healPartitions() {
    network_->healPartitions();
    recordAction(ConsensusActionType::HEAL_PARTITION);
}

void ConsensusSimulator::setNetworkLatency(uint64_t latency_ms) {
    network_->setGlobalLatency(latency_ms);
    recordAction(ConsensusActionType::SET_LATENCY);
}

void ConsensusSimulator::setPacketLoss(double loss_rate) {
    network_->setGlobalPacketLoss(loss_rate);
    recordAction(ConsensusActionType::SET_PACKET_LOSS);
}

// ============================================================================
// Blockchain Simulation
// ============================================================================

void ConsensusSimulator::simulateBlockMined(const NodeID& node_id, const std::string& block_hash) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    ConsensusNode* node = it->second.get();

    // Record BLOCK_MINED action
    recordAction(ConsensusActionType::BLOCK_MINED, node_id, block_hash);

    // Node accepts its own block
    node->receiveBlock(block_hash, node_id);

    // Broadcast to all peers
    auto peers = node->broadcastBlock(block_hash);

    // Network routes messages
    for (const auto& peer_id : peers) {
        network_->sendMessage(
            node_id,
            peer_id,
            ConsensusEventType::BLOCK_RECEIVED,
            block_hash,
            current_time_
        );
    }

    recordAction(ConsensusActionType::BROADCAST_BLOCK, node_id, block_hash);
}

void ConsensusSimulator::broadcastTransaction(const NodeID& node_id, const std::string& tx_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    ConsensusNode* node = it->second.get();

    // Record BROADCAST_TX action
    recordAction(ConsensusActionType::BROADCAST_TX, node_id, tx_id);

    // Node accepts its own tx
    node->receiveTransaction(tx_id, node_id);

    // Broadcast to all peers
    auto peers = node->broadcastTransaction(tx_id);

    // Network routes messages
    for (const auto& peer_id : peers) {
        network_->sendMessage(
            node_id,
            peer_id,
            ConsensusEventType::TX_RECEIVED,
            tx_id,
            current_time_
        );
    }
}

// ============================================================================
// Byzantine Control
// ============================================================================

void ConsensusSimulator::enableByzantine(const NodeID& node_id, const ByzantineStrategy& strategy) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    it->second->enableByzantine(strategy);
    recordAction(ConsensusActionType::ENABLE_BYZANTINE, node_id);
}

void ConsensusSimulator::disableByzantine(const NodeID& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        return;
    }

    it->second->disableByzantine();
    recordAction(ConsensusActionType::DISABLE_BYZANTINE, node_id);
}

// ============================================================================
// Trace Access
// ============================================================================

ConsensusTrace ConsensusSimulator::getTrace() const {
    ConsensusTrace trace;
    trace.rng_seed = rng_seed_;
    trace.scenario_name = scenario_name_;
    trace.topology = topology_;
    trace.actions = actions_;
    trace.start_time = 0;
    trace.end_time = current_time_;
    trace.completed_successfully = running_;

    // Collect all events from all nodes
    for (const auto& [node_id, node] : nodes_) {
        const auto& node_events = node->getEvents();
        trace.events.insert(trace.events.end(), node_events.begin(), node_events.end());
    }

    // Copy snapshots
    trace.snapshots = snapshots_;

    // Compute trace hash (for DD1 determinism oracle)
    trace.final_hash = trace.computeHash();

    return trace;
}

void ConsensusSimulator::captureSnapshots() {
    for (const auto& [node_id, node] : nodes_) {
        ConsensusState snapshot = node->captureState(current_time_);
        snapshots_.push_back(snapshot);
    }
}

// ============================================================================
// Statistics
// ============================================================================

size_t ConsensusSimulator::getTotalEventCount() const {
    size_t total = 0;
    for (const auto& [node_id, node] : nodes_) {
        total += node->getEvents().size();
    }
    return total;
}

size_t ConsensusSimulator::getTotalSnapshotCount() const {
    return snapshots_.size();
}

ConsensusSimulator::NetworkStats ConsensusSimulator::getNetworkStats() const {
    NetworkStats stats;
    stats.messages_sent = network_->getTotalMessagesSent();
    stats.messages_delivered = network_->getTotalMessagesDelivered();
    stats.messages_dropped = network_->getTotalMessagesDropped();
    stats.messages_blocked = network_->getTotalMessagesBlocked();
    stats.pending_messages = network_->getPendingMessageCount();
    return stats;
}

// ============================================================================
// Helpers
// ============================================================================

void ConsensusSimulator::recordAction(
    ConsensusActionType type,
    const std::optional<NodeID>& node_id,
    const std::optional<std::string>& payload
) {
    ConsensusAction action;
    action.type = type;
    action.timestamp = current_time_;
    action.sequence_number = action_sequence_++;
    action.node_id = node_id;

    // Set payload based on action type
    if (payload) {
        if (type == ConsensusActionType::BLOCK_MINED ||
            type == ConsensusActionType::BROADCAST_BLOCK) {
            action.block_hash = *payload;
        } else if (type == ConsensusActionType::BROADCAST_TX) {
            action.tx_id = *payload;
        }
    }

    actions_.push_back(action);
}

void ConsensusSimulator::collectEvents() {
    // Events are collected in getTrace() by reading from nodes
    // This is a placeholder for future functionality
}

void ConsensusSimulator::establishConnections() {
    // Connect nodes per topology
    for (const auto& [node_id, peers] : topology_.connections) {
        auto node_it = nodes_.find(node_id);
        if (node_it == nodes_.end()) {
            continue;
        }

        ConsensusNode* node = node_it->second.get();
        for (const auto& peer_id : peers) {
            node->connectToPeer(peer_id);
        }
    }
}

} // namespace test
} // namespace consensus
} // namespace dinero
