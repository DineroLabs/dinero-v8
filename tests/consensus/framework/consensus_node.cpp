#include "consensus_node.h"
#include <algorithm>

namespace dinero {
namespace consensus {
namespace test {

ConsensusNode::ConsensusNode(
    const NodeID& node_id,
    const ChainParams& params,
    uint64_t rng_seed
)
    : node_id_(node_id)
    , params_(params)
    , rng_seed_(rng_seed)
    , running_(false)
    , chain_tip_hash_("genesis")
    , chain_height_(0)
    , chainwork_(0)
    , is_mining_(false)
    , is_syncing_(false)
    , is_byzantine_(false)
    , event_sequence_(0)
{
    // Mining simulator will be integrated in Phase 5b when needed
    // For Phase 5a foundation, we use simplified chain state
}

ConsensusNode::~ConsensusNode() {
    if (running_) {
        stop();
    }
}

// ============================================================================
// Node Lifecycle
// ============================================================================

void ConsensusNode::start() {
    if (running_) {
        return;
    }

    running_ = true;

    // Initialize chain state (genesis block)
    chain_tip_hash_ = "genesis";
    chain_height_ = 0;
    chainwork_ = 0;

    // Clear mempool
    mempool_txids_.clear();

    // Record NODE_START event
    recordEventInternal(ConsensusEventType::MINING_STARTED, 0, true);
}

void ConsensusNode::stop() {
    if (!running_) {
        return;
    }

    // Stop mining if active
    if (is_mining_) {
        stopMining();
    }

    // Disconnect from all peers
    connected_peers_.clear();

    running_ = false;

    // Record NODE_STOP event
    recordEventInternal(ConsensusEventType::MINING_STOPPED, 0, true);
}

// ============================================================================
// P2P Connectivity
// ============================================================================

void ConsensusNode::connectToPeer(const NodeID& peer_id) {
    if (!running_) {
        return;
    }

    // Check if already connected
    auto it = std::find(connected_peers_.begin(), connected_peers_.end(), peer_id);
    if (it != connected_peers_.end()) {
        return;  // Already connected
    }

    connected_peers_.push_back(peer_id);

    // Record PEER_CONNECTED event
    ConsensusEvent event;
    event.type = ConsensusEventType::PEER_CONNECTED;
    event.timestamp = 0;  // Will be set by simulator
    event.sequence_number = event_sequence_++;
    event.node_id = node_id_;
    event.peer_id = peer_id;
    event.success = true;
    recordEvent(event);
}

void ConsensusNode::disconnectFromPeer(const NodeID& peer_id) {
    auto it = std::find(connected_peers_.begin(), connected_peers_.end(), peer_id);
    if (it == connected_peers_.end()) {
        return;  // Not connected
    }

    connected_peers_.erase(it);

    // Record PEER_DISCONNECTED event
    ConsensusEvent event;
    event.type = ConsensusEventType::PEER_DISCONNECTED;
    event.timestamp = 0;
    event.sequence_number = event_sequence_++;
    event.node_id = node_id_;
    event.peer_id = peer_id;
    event.success = true;
    recordEvent(event);
}

bool ConsensusNode::isConnectedTo(const NodeID& peer_id) const {
    return std::find(connected_peers_.begin(), connected_peers_.end(), peer_id)
           != connected_peers_.end();
}

std::vector<NodeID> ConsensusNode::getConnectedPeers() const {
    return connected_peers_;
}

// ============================================================================
// Mining Control
// ============================================================================

void ConsensusNode::startMining() {
    if (!running_ || is_mining_) {
        return;
    }

    is_mining_ = true;

    // Record MINING_STARTED event
    recordEventInternal(ConsensusEventType::MINING_STARTED, 0, true);
}

void ConsensusNode::stopMining() {
    if (!is_mining_) {
        return;
    }

    is_mining_ = false;

    // Record MINING_STOPPED event
    recordEventInternal(ConsensusEventType::MINING_STOPPED, 0, true);
}

bool ConsensusNode::isMining() const {
    return is_mining_;
}

MiningPhase ConsensusNode::getMiningPhase() const {
    if (!is_mining_) {
        return MiningPhase::STOPPED;
    }
    return MiningPhase::MINING;  // Simplified for now
}

// ============================================================================
// Blockchain Operations
// ============================================================================

bool ConsensusNode::receiveBlock(const std::string& block_hash, const NodeID& from_peer) {
    if (!running_) {
        return false;
    }

    // Byzantine behavior: reject blocks if Byzantine
    if (is_byzantine_ && byzantine_strategy_) {
        if (byzantine_strategy_->type == ByzantineStrategyType::BLOCK_WITHHOLDER) {
            // Withholders reject all blocks
            recordEventInternal(ConsensusEventType::BLOCK_REJECTED, 0, false, "Byzantine withholding");
            return false;
        }
    }

    // Record BLOCK_RECEIVED event
    ConsensusEvent event;
    event.type = ConsensusEventType::BLOCK_RECEIVED;
    event.timestamp = 0;
    event.sequence_number = event_sequence_++;
    event.node_id = node_id_;
    event.peer_id = from_peer;
    event.block_hash = block_hash;
    event.success = true;
    recordEvent(event);

    // Simplified validation: accept all blocks from honest peers
    // Real validation will use Ring 2's validation in Phase 5b
    bool accepted = true;

    if (accepted) {
        // Update chain state (simplified)
        chain_tip_hash_ = block_hash;
        chain_height_++;
        chainwork_ += 1;  // Simplified

        // Record BLOCK_ACCEPTED event
        ConsensusEvent accept_event;
        accept_event.type = ConsensusEventType::BLOCK_ACCEPTED;
        accept_event.timestamp = 0;
        accept_event.sequence_number = event_sequence_++;
        accept_event.node_id = node_id_;
        accept_event.block_hash = block_hash;
        accept_event.block_height = chain_height_;
        accept_event.chainwork = chainwork_;
        accept_event.success = true;
        recordEvent(accept_event);

        // Record CHAIN_TIP_CHANGED event
        ConsensusEvent tip_event;
        tip_event.type = ConsensusEventType::CHAIN_TIP_CHANGED;
        tip_event.timestamp = 0;
        tip_event.sequence_number = event_sequence_++;
        tip_event.node_id = node_id_;
        tip_event.block_hash = block_hash;
        tip_event.block_height = chain_height_;
        tip_event.chainwork = chainwork_;
        tip_event.success = true;
        recordEvent(tip_event);
    }

    return accepted;
}

std::vector<NodeID> ConsensusNode::broadcastBlock(const std::string& block_hash) {
    if (!running_) {
        return {};
    }

    // Byzantine behavior: withhold blocks if selfish miner
    if (is_byzantine_ && byzantine_strategy_) {
        if (byzantine_strategy_->type == ByzantineStrategyType::SELFISH_MINER) {
            // Don't broadcast (withhold)
            return {};
        }
    }

    // Broadcast to all connected peers
    std::vector<NodeID> sent_to;
    for (const auto& peer_id : connected_peers_) {
        // Record MESSAGE_SENT event
        ConsensusEvent event;
        event.type = ConsensusEventType::MESSAGE_SENT;
        event.timestamp = 0;
        event.sequence_number = event_sequence_++;
        event.node_id = node_id_;
        event.peer_id = peer_id;
        event.block_hash = block_hash;
        event.message_type = "block";
        event.success = true;
        recordEvent(event);

        sent_to.push_back(peer_id);
    }

    return sent_to;
}

bool ConsensusNode::receiveTransaction(const std::string& tx_id, const NodeID& from_peer) {
    if (!running_) {
        return false;
    }

    // Record TX_RECEIVED event
    ConsensusEvent event;
    event.type = ConsensusEventType::TX_RECEIVED;
    event.timestamp = 0;
    event.sequence_number = event_sequence_++;
    event.node_id = node_id_;
    event.peer_id = from_peer;
    event.tx_id = tx_id;
    event.success = true;
    recordEvent(event);

    // Add to mempool (simplified)
    mempool_txids_.push_back(tx_id);

    // Record TX_ACCEPTED event
    ConsensusEvent accept_event;
    accept_event.type = ConsensusEventType::TX_ACCEPTED;
    accept_event.timestamp = 0;
    accept_event.sequence_number = event_sequence_++;
    accept_event.node_id = node_id_;
    accept_event.tx_id = tx_id;
    accept_event.success = true;
    recordEvent(accept_event);

    return true;
}

std::vector<NodeID> ConsensusNode::broadcastTransaction(const std::string& tx_id) {
    if (!running_) {
        return {};
    }

    // Broadcast to all connected peers
    std::vector<NodeID> sent_to;
    for (const auto& peer_id : connected_peers_) {
        // Record MESSAGE_SENT event
        ConsensusEvent event;
        event.type = ConsensusEventType::MESSAGE_SENT;
        event.timestamp = 0;
        event.sequence_number = event_sequence_++;
        event.node_id = node_id_;
        event.peer_id = peer_id;
        event.tx_id = tx_id;
        event.message_type = "tx";
        event.success = true;
        recordEvent(event);

        sent_to.push_back(peer_id);
    }

    return sent_to;
}

// ============================================================================
// State Queries
// ============================================================================

std::string ConsensusNode::getChainTipHash() const {
    return chain_tip_hash_;
}

uint32_t ConsensusNode::getChainHeight() const {
    return chain_height_;
}

uint64_t ConsensusNode::getChainwork() const {
    return chainwork_;
}

size_t ConsensusNode::getMempoolSize() const {
    return mempool_txids_.size();
}

std::vector<std::string> ConsensusNode::getMempoolTxids() const {
    return mempool_txids_;
}

ConsensusState ConsensusNode::captureState(uint64_t timestamp) const {
    ConsensusState state;
    state.node_id = node_id_;
    state.timestamp = timestamp;
    state.chain_tip_hash = chain_tip_hash_;
    state.chain_height = chain_height_;
    state.chainwork = chainwork_;
    state.mempool_txids = mempool_txids_;
    state.mempool_size_bytes = mempool_txids_.size() * 250;  // Simplified
    state.connected_peers = connected_peers_;
    state.peer_count = connected_peers_.size();
    state.mining_phase = getMiningPhase();
    state.is_syncing = is_syncing_;
    state.sync_target_height = sync_target_height_;
    state.sync_peer = sync_peer_;
    state.is_byzantine = is_byzantine_;
    state.byzantine_strategy = is_byzantine_ && byzantine_strategy_
        ? std::to_string(static_cast<int>(byzantine_strategy_->type))
        : "";

    return state;
}

// ============================================================================
// Byzantine Behavior
// ============================================================================

void ConsensusNode::enableByzantine(const ByzantineStrategy& strategy) {
    is_byzantine_ = true;
    byzantine_strategy_ = strategy;
}

void ConsensusNode::disableByzantine() {
    is_byzantine_ = false;
    byzantine_strategy_.reset();
}

std::optional<ByzantineStrategy> ConsensusNode::getByzantineStrategy() const {
    return byzantine_strategy_;
}

// ============================================================================
// Message Queue (Network Simulation)
// ============================================================================

void ConsensusNode::enqueueMessage(
    const NodeID& from_peer,
    ConsensusEventType message_type,
    const std::string& payload,
    uint64_t timestamp
) {
    PendingMessage msg;
    msg.from_peer = from_peer;
    msg.type = message_type;
    msg.payload = payload;
    msg.timestamp = timestamp;
    message_queue_.push_back(msg);
}

void ConsensusNode::processMessages(uint64_t current_time) {
    // Process all pending messages up to current_time
    std::vector<PendingMessage> remaining;

    for (const auto& msg : message_queue_) {
        if (msg.timestamp <= current_time) {
            // Process message
            if (msg.type == ConsensusEventType::BLOCK_RECEIVED) {
                receiveBlock(msg.payload, msg.from_peer);
            } else if (msg.type == ConsensusEventType::TX_RECEIVED) {
                receiveTransaction(msg.payload, msg.from_peer);
            }
        } else {
            // Message not ready yet
            remaining.push_back(msg);
        }
    }

    message_queue_ = remaining;
}

// ============================================================================
// Event Recording (for ConsensusTrace)
// ============================================================================

void ConsensusNode::recordEvent(ConsensusEvent event) {
    events_.push_back(event);
}

void ConsensusNode::recordEventInternal(
    ConsensusEventType type,
    uint64_t timestamp,
    bool success,
    const std::string& error_message
) {
    ConsensusEvent event;
    event.type = type;
    event.timestamp = timestamp;
    event.sequence_number = event_sequence_++;
    event.node_id = node_id_;
    event.success = success;
    event.error_message = error_message;
    recordEvent(event);
}

} // namespace test
} // namespace consensus
} // namespace dinero
