#include "consensus/sync_simulator.h"
#include <algorithm>

namespace dinero {
namespace consensus {

// SimulatedPeer implementation

bool SimulatedPeer::WillRespond(std::mt19937& rng) const {
    switch (behavior) {
        case PeerBehavior::HONEST:
            return true;
        case PeerBehavior::WITHHOLDING:
            return false;
        case PeerBehavior::SLOW:
            return false;  // Always timeout
        case PeerBehavior::FLAKY: {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(rng) > 0.5;  // 50% chance
        }
        case PeerBehavior::INVALID_PROOFS:
            return true;  // Responds, but with invalid data
        default:
            return false;
    }
}

std::optional<BlockUtreexoData> SimulatedPeer::GetProof(const uint256& block_hash, std::mt19937& rng) {
    // Check if peer has the proof
    auto it = available_proofs.find(block_hash);
    if (it == available_proofs.end()) {
        return std::nullopt;  // Don't have this proof
    }

    // Check if peer will respond
    if (!WillRespond(rng)) {
        return std::nullopt;
    }

    // INVALID_PROOFS behavior: return corrupted proof
    if (behavior == PeerBehavior::INVALID_PROOFS) {
        BlockUtreexoData invalid_proof = it->second;
        // Corrupt the proof by clearing targets
        invalid_proof.spend_proof.targets.clear();
        return invalid_proof;
    }

    // Return valid proof
    return it->second;
}

// SimulatedNetwork implementation

SimulatedNetwork::SimulatedNetwork() {
    // Default: 100ms constant latency, no packet loss
    latency_model_ = std::make_unique<ConstantLatency>(100);
}

void SimulatedNetwork::SetLatencyModel(std::unique_ptr<LatencyModel> model) {
    std::lock_guard<std::mutex> lock(mutex_);
    latency_model_ = std::move(model);
}

void SimulatedNetwork::SetPacketLoss(double loss_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    packet_loss_rate_ = std::clamp(loss_rate, 0.0, 1.0);
}

bool SimulatedNetwork::SendMessage(
    uint64_t from_node,
    uint64_t to_node,
    size_t message_size,
    uint64_t& delivery_time,
    SimTime current_time,
    std::mt19937& rng) {

    std::lock_guard<std::mutex> lock(mutex_);

    stats_.packets_sent++;
    stats_.bytes_sent += message_size;

    // Check packet loss
    if (packet_loss_rate_ > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) < packet_loss_rate_) {
            stats_.packets_dropped++;
            return false;  // Packet lost
        }
    }

    // Calculate delivery time
    uint64_t latency = latency_model_->GetLatency(rng);
    delivery_time = current_time + latency;

    stats_.packets_delivered++;
    stats_.bytes_delivered += message_size;

    return true;
}

NetworkStats SimulatedNetwork::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void SimulatedNetwork::ClearStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = NetworkStats();
}

// SimulatedNode implementation

SimulatedNode::SimulatedNode(uint64_t node_id, uint32_t target_height)
    : node_id_(node_id), target_height_(target_height) {
}

NodeState SimulatedNode::GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint32_t SimulatedNode::GetCurrentHeight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_height_;
}

void SimulatedNode::StartSync(SimTime current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = NodeState::SYNCING;
    stats_.sync_start_time = current_time;
}

bool SimulatedNode::RequestNextBlock(SimTime current_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (current_height_ >= target_height_) {
        // Sync complete
        state_ = NodeState::SYNCED;
        stats_.sync_end_time = current_time;
        return false;
    }

    stats_.proofs_requested++;
    return true;
}

void SimulatedNode::HandleProofResponse(const BlockUtreexoData& proof, bool valid, SimTime current_time) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!valid) {
        // Invalid proof rejected
        stats_.invalid_proofs_rejected++;
        stats_.proofs_failed++;
        return;
    }

    // Proof accepted - advance block height
    stats_.proofs_received++;
    current_height_++;
    stats_.blocks_synced++;

    // Check if sync complete
    if (current_height_ >= target_height_) {
        state_ = NodeState::SYNCED;
        stats_.sync_end_time = current_time;
    }
}

void SimulatedNode::HandleTimeout(SimTime current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.proofs_failed++;
}

SyncStats SimulatedNode::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool SimulatedNode::IsSyncComplete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == NodeState::SYNCED;
}

// SyncSimulator implementation

SyncSimulator::SyncSimulator() {
    // Default: no time limit, random seed
    time_limit_ = 0;
    rng_.seed(std::random_device{}());
}

void SyncSimulator::SetSeed(uint64_t seed) {
    rng_.seed(seed);
}

void SyncSimulator::SetTimeLimit(SimTime limit_ms) {
    time_limit_ = limit_ms;
}

void SyncSimulator::AddNode(uint32_t target_height) {
    auto node = std::make_unique<SimulatedNode>(next_node_id_++, target_height);
    nodes_.push_back(std::move(node));
}

void SyncSimulator::AddPeer(PeerBehavior behavior) {
    SimulatedPeer peer(next_peer_id_++, behavior);
    peers_.push_back(peer);
}

void SyncSimulator::SetNetwork(std::shared_ptr<SimulatedNetwork> network) {
    network_ = network;
}

void SyncSimulator::AddProofToAllPeers(const uint256& block_hash, const BlockUtreexoData& proof) {
    for (auto& peer : peers_) {
        // Only add to honest and flaky peers (not withholding)
        if (peer.behavior != PeerBehavior::WITHHOLDING) {
            peer.available_proofs[block_hash] = proof;
        }
    }
}

void SyncSimulator::PopulatePeerProofs(uint32_t max_height,
                                       std::function<BlockUtreexoData(uint32_t)> proof_generator,
                                       std::function<uint256(uint32_t)> hash_generator) {
    for (uint32_t height = 1; height <= max_height; height++) {
        uint256 block_hash = hash_generator(height);
        BlockUtreexoData proof = proof_generator(height);
        AddProofToAllPeers(block_hash, proof);
    }
}

SimulationResults SyncSimulator::Run() {
    // Initialize network if not set
    if (!network_) {
        network_ = std::make_shared<SimulatedNetwork>();
    }

    // Schedule node start events
    for (const auto& node : nodes_) {
        SimulationEvent start_event(0, EventType::NODE_START, node->GetNodeId());
        ScheduleEvent(start_event);
    }

    // Run simulation loop
    while (!event_queue_.empty()) {
        // Check time limit
        if (time_limit_ > 0 && current_time_ >= time_limit_) {
            break;
        }

        // Process next event
        if (!ProcessNextEvent()) {
            break;  // All nodes done
        }

        // Check if all nodes are done
        if (AllNodesDone()) {
            break;
        }
    }

    // Collect and return results
    return CollectResults();
}

void SyncSimulator::ScheduleEvent(const SimulationEvent& event) {
    event_queue_.push(event);
}

bool SyncSimulator::ProcessNextEvent() {
    if (event_queue_.empty()) {
        return false;
    }

    // Get next event
    SimulationEvent event = event_queue_.top();
    event_queue_.pop();

    // Advance simulation time
    current_time_ = event.time;

    // Handle event by type
    switch (event.type) {
        case EventType::NODE_START:
            HandleNodeStart(event);
            break;
        case EventType::NODE_SYNC_BLOCK:
            HandleSyncBlock(event);
            break;
        case EventType::PROOF_REQUEST:
            HandleProofRequest(event);
            break;
        case EventType::PROOF_RESPONSE:
            HandleProofResponse(event);
            break;
        case EventType::TIMEOUT:
            HandleTimeout(event);
            break;
        case EventType::NODE_COMPLETE:
            // Nothing to do
            break;
    }

    return true;
}

void SyncSimulator::HandleNodeStart(SimulationEvent& event) {
    // Find node
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const auto& node) { return node->GetNodeId() == event.node_id; });

    if (it == nodes_.end()) return;

    // Start syncing
    (*it)->StartSync(current_time_);

    // Schedule first block sync
    SimulationEvent sync_event(current_time_, EventType::NODE_SYNC_BLOCK, event.node_id);
    ScheduleEvent(sync_event);
}

void SyncSimulator::HandleSyncBlock(SimulationEvent& event) {
    // Find node
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const auto& node) { return node->GetNodeId() == event.node_id; });

    if (it == nodes_.end()) return;

    SimulatedNode* node = it->get();

    // Request next block
    if (!node->RequestNextBlock(current_time_)) {
        // Sync complete
        SimulationEvent complete_event(current_time_, EventType::NODE_COMPLETE, event.node_id);
        ScheduleEvent(complete_event);
        return;
    }

    // Select peer for proof request
    if (peers_.empty()) {
        // No peers available - schedule retry
        SimulationEvent retry_event(current_time_ + 1000, EventType::NODE_SYNC_BLOCK, event.node_id);
        ScheduleEvent(retry_event);
        return;
    }

    uint64_t peer_id = SelectPeer();

    // Schedule proof request (immediate)
    SimulationEvent request_event(current_time_, EventType::PROOF_REQUEST, event.node_id);
    request_event.peer_id = peer_id;
    ScheduleEvent(request_event);

    // Schedule timeout (5 seconds)
    SimulationEvent timeout_event(current_time_ + 5000, EventType::TIMEOUT, event.node_id);
    timeout_event.peer_id = peer_id;
    ScheduleEvent(timeout_event);
}

void SyncSimulator::HandleProofRequest(SimulationEvent& event) {
    // Find peer
    auto peer_it = std::find_if(peers_.begin(), peers_.end(),
        [&](const auto& peer) { return peer.peer_id == event.peer_id; });

    if (peer_it == peers_.end()) return;

    // Find node to get current height
    auto node_it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const auto& node) { return node->GetNodeId() == event.node_id; });

    if (node_it == nodes_.end()) return;

    // Calculate block hash for next block (current_height + 1)
    uint32_t next_height = (*node_it)->GetCurrentHeight() + 1;
    uint256 block_hash;
    block_hash.data[0] = static_cast<uint8_t>(next_height & 0xFF);
    block_hash.data[1] = static_cast<uint8_t>((next_height >> 8) & 0xFF);
    block_hash.data[2] = static_cast<uint8_t>((next_height >> 16) & 0xFF);
    block_hash.data[3] = static_cast<uint8_t>((next_height >> 24) & 0xFF);
    for (size_t i = 4; i < 32; i++) {
        block_hash.data[i] = static_cast<uint8_t>((next_height + i) & 0xFF);
    }

    // Get proof from peer
    auto proof = peer_it->GetProof(block_hash, rng_);

    if (!proof.has_value()) {
        // Peer didn't respond - timeout will trigger retry
        return;
    }

    // Send proof response via network
    uint64_t delivery_time;
    bool delivered = network_->SendMessage(
        event.peer_id,
        event.node_id,
        1000,  // Assume 1 KB proof
        delivery_time,
        current_time_,
        rng_);

    if (!delivered) {
        // Packet lost - timeout will trigger retry
        return;
    }

    // Schedule proof response delivery
    SimulationEvent response_event(delivery_time, EventType::PROOF_RESPONSE, event.node_id);
    response_event.peer_id = event.peer_id;
    response_event.proof_data = proof.value();
    ScheduleEvent(response_event);
}

void SyncSimulator::HandleProofResponse(SimulationEvent& event) {
    // Find node
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const auto& node) { return node->GetNodeId() == event.node_id; });

    if (it == nodes_.end()) return;

    // Validate proof (simple check: proof has targets)
    bool valid = !event.proof_data.spend_proof.targets.empty();

    // Handle proof response
    (*it)->HandleProofResponse(event.proof_data, valid, current_time_);

    // Schedule next block sync
    SimulationEvent sync_event(current_time_, EventType::NODE_SYNC_BLOCK, event.node_id);
    ScheduleEvent(sync_event);
}

void SyncSimulator::HandleTimeout(SimulationEvent& event) {
    // Find node
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&](const auto& node) { return node->GetNodeId() == event.node_id; });

    if (it == nodes_.end()) return;

    // Check if node is still syncing (response may have arrived)
    if ((*it)->IsSyncComplete()) {
        return;  // Already completed, ignore timeout
    }

    // Handle timeout
    (*it)->HandleTimeout(current_time_);

    // Retry with different peer
    SimulationEvent retry_event(current_time_ + 100, EventType::NODE_SYNC_BLOCK, event.node_id);
    ScheduleEvent(retry_event);
}

uint64_t SyncSimulator::SelectPeer() {
    if (peers_.empty()) return 0;

    // Simple random selection
    std::uniform_int_distribution<size_t> dist(0, peers_.size() - 1);
    size_t idx = dist(rng_);
    return peers_[idx].peer_id;
}

bool SyncSimulator::AllNodesDone() const {
    for (const auto& node : nodes_) {
        NodeState state = node->GetState();
        if (state != NodeState::SYNCED && state != NodeState::FAILED) {
            return false;
        }
    }
    return true;
}

SimulationResults SyncSimulator::CollectResults() {
    SimulationResults results;
    results.total_simulation_time = current_time_;

    // Collect per-node stats
    for (const auto& node : nodes_) {
        SyncStats stats = node->GetStats();
        results.node_stats.push_back(stats);

        if (node->IsSyncComplete()) {
            results.nodes_synced++;
        } else {
            results.nodes_failed++;
        }
    }

    // Network stats
    results.network_stats = network_->GetStats();

    return results;
}

} // namespace consensus
} // namespace dinero
