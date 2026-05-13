#pragma once

#include <queue>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <random>
#include "primitives/uint256.h"
#include "primitives/block.h"
#include "consensus/lightning_proof_client.h"

namespace dinero {
namespace consensus {

// Forward declarations
class SimulatedNode;
class SimulatedNetwork;
class SyncSimulator;

/**
 * Simulation time (milliseconds since simulation start)
 */
using SimTime = uint64_t;

/**
 * Network latency model
 *
 * Phase 10.2: Models realistic WAN conditions
 */
class LatencyModel {
public:
    virtual ~LatencyModel() = default;

    /**
     * Get latency for this packet
     * @param rng Random number generator
     * @return Latency in milliseconds
     */
    virtual uint64_t GetLatency(std::mt19937& rng) = 0;

    /**
     * Get model name for debugging
     */
    virtual const char* GetName() const = 0;
};

/**
 * Constant latency (e.g., LAN)
 */
class ConstantLatency : public LatencyModel {
public:
    explicit ConstantLatency(uint64_t latency_ms) : latency_ms_(latency_ms) {}

    uint64_t GetLatency(std::mt19937& rng) override {
        return latency_ms_;
    }

    const char* GetName() const override { return "Constant"; }

private:
    uint64_t latency_ms_;
};

/**
 * Uniform random latency (e.g., variable WAN)
 */
class UniformLatency : public LatencyModel {
public:
    UniformLatency(uint64_t min_ms, uint64_t max_ms)
        : min_ms_(min_ms), max_ms_(max_ms) {}

    uint64_t GetLatency(std::mt19937& rng) override {
        std::uniform_int_distribution<uint64_t> dist(min_ms_, max_ms_);
        return dist(rng);
    }

    const char* GetName() const override { return "Uniform"; }

private:
    uint64_t min_ms_;
    uint64_t max_ms_;
};

/**
 * Simulated peer behavior
 *
 * Phase 10.3: Models adversarial and honest peers
 */
enum class PeerBehavior {
    HONEST,              // Always provides correct proofs
    WITHHOLDING,         // Never sends proofs
    INVALID_PROOFS,      // Sends corrupted proofs
    SLOW,                // Always times out
    FLAKY                // Intermittently fails (50%)
};

/**
 * Simulated peer
 */
struct SimulatedPeer {
    uint64_t peer_id;
    PeerBehavior behavior;
    std::unordered_map<uint256, BlockUtreexoData> available_proofs;

    SimulatedPeer(uint64_t id, PeerBehavior b)
        : peer_id(id), behavior(b) {}

    /**
     * Check if this peer will respond to proof request
     */
    bool WillRespond(std::mt19937& rng) const;

    /**
     * Get proof from this peer (may be invalid/missing based on behavior)
     */
    std::optional<BlockUtreexoData> GetProof(const uint256& block_hash, std::mt19937& rng);
};

/**
 * Network statistics
 */
struct NetworkStats {
    uint64_t packets_sent = 0;
    uint64_t packets_delivered = 0;
    uint64_t packets_dropped = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_delivered = 0;

    double PacketLossRate() const {
        if (packets_sent == 0) return 0.0;
        return static_cast<double>(packets_dropped) / packets_sent;
    }
};

/**
 * Simulated network layer
 *
 * Phase 10.2: Models network conditions (latency, packet loss)
 */
class SimulatedNetwork {
public:
    SimulatedNetwork();
    ~SimulatedNetwork() = default;

    /**
     * Set latency model
     */
    void SetLatencyModel(std::unique_ptr<LatencyModel> model);

    /**
     * Set packet loss rate (0.0 = no loss, 1.0 = 100% loss)
     */
    void SetPacketLoss(double loss_rate);

    /**
     * Send message with network delay
     * @param from_node Sender node ID
     * @param to_node Recipient node ID
     * @param message_size Size in bytes
     * @param delivery_time Output: when message will be delivered
     * @param rng Random number generator
     * @return true if packet will be delivered, false if dropped
     */
    bool SendMessage(
        uint64_t from_node,
        uint64_t to_node,
        size_t message_size,
        uint64_t& delivery_time,
        SimTime current_time,
        std::mt19937& rng);

    /**
     * Get network statistics
     */
    NetworkStats GetStats() const;

    /**
     * Clear statistics
     */
    void ClearStats();

private:
    std::unique_ptr<LatencyModel> latency_model_;
    double packet_loss_rate_ = 0.0;
    NetworkStats stats_;
    mutable std::mutex mutex_;
};

/**
 * Simulation event types
 */
enum class EventType {
    NODE_START,          // Node initialization
    NODE_SYNC_BLOCK,     // Node requests next block
    PROOF_REQUEST,       // Node requests proof from peer
    PROOF_RESPONSE,      // Peer responds with proof
    TIMEOUT,             // Request timeout
    NODE_COMPLETE        // Node sync complete
};

/**
 * Simulation event
 */
struct SimulationEvent {
    SimTime time;
    EventType type;
    uint64_t node_id;
    uint64_t peer_id;  // For proof request/response
    uint256 block_hash;
    BlockUtreexoData proof_data;

    SimulationEvent(SimTime t, EventType et, uint64_t nid)
        : time(t), type(et), node_id(nid), peer_id(0) {}

    // Comparison for priority queue (earlier time = higher priority)
    bool operator>(const SimulationEvent& other) const {
        return time > other.time;
    }
};

/**
 * Simulated node state
 */
enum class NodeState {
    INITIALIZING,
    SYNCING,
    SYNCED,
    FAILED
};

/**
 * Sync statistics per node
 */
struct SyncStats {
    uint32_t blocks_synced = 0;
    uint32_t proofs_requested = 0;
    uint32_t proofs_received = 0;
    uint32_t proofs_failed = 0;
    uint32_t invalid_proofs_rejected = 0;
    SimTime sync_start_time = 0;
    SimTime sync_end_time = 0;

    uint64_t SyncDuration() const {
        if (sync_end_time == 0) return 0;
        return sync_end_time - sync_start_time;
    }

    double SuccessRate() const {
        if (proofs_requested == 0) return 0.0;
        return static_cast<double>(proofs_received) / proofs_requested;
    }
};

/**
 * Simulated stateless node
 *
 * Phase 10.1: Node that syncs blockchain using proof requests
 */
class SimulatedNode {
public:
    SimulatedNode(uint64_t node_id, uint32_t target_height);
    ~SimulatedNode() = default;

    /**
     * Get node ID
     */
    uint64_t GetNodeId() const { return node_id_; }

    /**
     * Get current state
     */
    NodeState GetState() const;

    /**
     * Get current block height
     */
    uint32_t GetCurrentHeight() const;

    /**
     * Get target height
     */
    uint32_t GetTargetHeight() const { return target_height_; }

    /**
     * Start syncing
     */
    void StartSync(SimTime current_time);

    /**
     * Request next block
     * @return true if more blocks to sync, false if complete
     */
    bool RequestNextBlock(SimTime current_time);

    /**
     * Handle proof response
     */
    void HandleProofResponse(const BlockUtreexoData& proof, bool valid, SimTime current_time);

    /**
     * Handle timeout
     */
    void HandleTimeout(SimTime current_time);

    /**
     * Get sync statistics
     */
    SyncStats GetStats() const;

    /**
     * Check if sync is complete
     */
    bool IsSyncComplete() const;

private:
    uint64_t node_id_;
    uint32_t current_height_ = 0;
    uint32_t target_height_;
    NodeState state_ = NodeState::INITIALIZING;
    SyncStats stats_;
    mutable std::mutex mutex_;
};

/**
 * Sync simulation results
 */
struct SimulationResults {
    SimTime total_simulation_time = 0;
    uint32_t nodes_synced = 0;
    uint32_t nodes_failed = 0;
    std::vector<SyncStats> node_stats;
    NetworkStats network_stats;

    double SuccessRate() const {
        uint32_t total_nodes = nodes_synced + nodes_failed;
        if (total_nodes == 0) return 0.0;
        return static_cast<double>(nodes_synced) / total_nodes;
    }

    uint64_t AverageSyncTime() const {
        if (node_stats.empty()) return 0;
        uint64_t total = 0;
        for (const auto& stats : node_stats) {
            total += stats.SyncDuration();
        }
        return total / node_stats.size();
    }
};

/**
 * Multi-node sync simulator
 *
 * Phase 10: Test stateless node sync under realistic conditions
 */
class SyncSimulator {
public:
    SyncSimulator();
    ~SyncSimulator() = default;

    /**
     * Set random seed for reproducibility
     */
    void SetSeed(uint64_t seed);

    /**
     * Set simulation time limit (milliseconds)
     */
    void SetTimeLimit(SimTime limit_ms);

    /**
     * Add simulated node
     */
    void AddNode(uint32_t target_height);

    /**
     * Add simulated peer
     */
    void AddPeer(PeerBehavior behavior);

    /**
     * Set network layer
     */
    void SetNetwork(std::shared_ptr<SimulatedNetwork> network);

    /**
     * Add proof to all honest peers
     */
    void AddProofToAllPeers(const uint256& block_hash, const BlockUtreexoData& proof);

    /**
     * Populate all honest peers with proofs up to max_height
     */
    void PopulatePeerProofs(uint32_t max_height,
                            std::function<BlockUtreexoData(uint32_t)> proof_generator,
                            std::function<uint256(uint32_t)> hash_generator);

    /**
     * Run simulation
     * @return Simulation results
     */
    SimulationResults Run();

    /**
     * Get current simulation time
     */
    SimTime GetCurrentTime() const { return current_time_; }

private:
    // Event queue (priority queue, earliest time first)
    std::priority_queue<SimulationEvent, std::vector<SimulationEvent>, std::greater<SimulationEvent>> event_queue_;

    // Simulation state
    SimTime current_time_ = 0;
    SimTime time_limit_ = 0;
    std::mt19937 rng_;

    // Network and peers
    std::shared_ptr<SimulatedNetwork> network_;
    std::vector<std::unique_ptr<SimulatedNode>> nodes_;
    std::vector<SimulatedPeer> peers_;

    // Statistics
    uint32_t next_node_id_ = 1;
    uint32_t next_peer_id_ = 1;

    /**
     * Schedule event
     */
    void ScheduleEvent(const SimulationEvent& event);

    /**
     * Process next event
     * @return true if simulation should continue, false if done
     */
    bool ProcessNextEvent();

    /**
     * Handle node start event
     */
    void HandleNodeStart(SimulationEvent& event);

    /**
     * Handle sync block event
     */
    void HandleSyncBlock(SimulationEvent& event);

    /**
     * Handle proof request event
     */
    void HandleProofRequest(SimulationEvent& event);

    /**
     * Handle proof response event
     */
    void HandleProofResponse(SimulationEvent& event);

    /**
     * Handle timeout event
     */
    void HandleTimeout(SimulationEvent& event);

    /**
     * Select random peer for proof request
     */
    uint64_t SelectPeer();

    /**
     * Check if all nodes are done (synced or failed)
     */
    bool AllNodesDone() const;

    /**
     * Collect simulation results
     */
    SimulationResults CollectResults();
};

} // namespace consensus
} // namespace dinero
