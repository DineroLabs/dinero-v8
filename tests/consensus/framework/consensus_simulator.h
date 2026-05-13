#pragma once

#include "consensus_types.h"
#include "consensus_trace.h"
#include "consensus_node.h"
#include "network_simulator.h"
#include <memory>
#include <map>
#include <vector>
#include <functional>

namespace dinero {
namespace consensus {
namespace test {

/**
 * ConsensusSimulator - Multi-node consensus simulation orchestrator
 *
 * Coordinates:
 * - Multiple ConsensusNodes (each with independent chain state)
 * - NetworkSimulator (message routing, partitions)
 * - Time progression (deterministic tick-based)
 * - Trace recording (all actions, events, state snapshots)
 *
 * Design Pattern (following Ring 4 MiningSimulator):
 * - Simulator executes actions → generates events → captures snapshots
 * - Oracle reads trace → observes events → reports violations
 *
 * Usage:
 *   ConsensusSimulator sim(topology, params, seed);
 *   sim.start();
 *   sim.tick(100);  // Advance 100ms
 *   sim.nodeStartMining("alice");
 *   sim.tick(1000);
 *   auto trace = sim.getTrace();
 *   // Pass trace to oracle for analysis
 */
class ConsensusSimulator {
public:
    /**
     * Create consensus simulator
     *
     * @param topology Network connectivity graph
     * @param params Blockchain parameters (regtest/mainnet)
     * @param rng_seed Deterministic RNG seed
     * @param scenario_name Test identification (for trace)
     */
    ConsensusSimulator(
        const NetworkTopology& topology,
        const dinero::ChainParams& params,
        uint64_t rng_seed,
        const std::string& scenario_name = "default"
    );

    ~ConsensusSimulator();

    // ========================================================================
    // Simulation Lifecycle
    // ========================================================================

    /**
     * Start simulation (initialize all nodes, establish connections)
     */
    void start();

    /**
     * Stop simulation (halt all nodes)
     */
    void stop();

    /**
     * Advance simulation time by delta milliseconds
     *
     * This is the main event loop:
     * - Process pending messages
     * - Update node states
     * - Capture snapshots (if interval elapsed)
     */
    void tick(uint64_t delta_ms);

    /**
     * Run simulation for duration (calls tick in loop)
     */
    void run(uint64_t duration_ms, uint64_t tick_interval_ms = 10);

    /**
     * Get current simulation time
     */
    uint64_t getCurrentTime() const { return current_time_; }

    // ========================================================================
    // Node Control
    // ========================================================================

    /**
     * Start node (join network)
     */
    void nodeStart(const NodeID& node_id);

    /**
     * Stop node (leave network)
     */
    void nodeStop(const NodeID& node_id);

    /**
     * Start mining on node
     */
    void nodeStartMining(const NodeID& node_id);

    /**
     * Stop mining on node
     */
    void nodeStopMining(const NodeID& node_id);

    /**
     * Get node (for direct access)
     */
    ConsensusNode* getNode(const NodeID& node_id);

    // ========================================================================
    // Network Control
    // ========================================================================

    /**
     * Partition network into groups
     */
    void partitionNetwork(const std::vector<std::vector<NodeID>>& groups);

    /**
     * Heal partitions
     */
    void healPartitions();

    /**
     * Set global network latency
     */
    void setNetworkLatency(uint64_t latency_ms);

    /**
     * Set global packet loss rate
     */
    void setPacketLoss(double loss_rate);

    // ========================================================================
    // Blockchain Simulation
    // ========================================================================

    /**
     * Simulate block mined by node
     *
     * Node generates block and broadcasts to peers.
     *
     * @param node_id Miner node ID
     * @param block_hash Block identifier
     */
    void simulateBlockMined(const NodeID& node_id, const std::string& block_hash);

    /**
     * Broadcast transaction from node
     */
    void broadcastTransaction(const NodeID& node_id, const std::string& tx_id);

    // ========================================================================
    // Byzantine Control
    // ========================================================================

    /**
     * Enable Byzantine behavior on node
     */
    void enableByzantine(const NodeID& node_id, const ByzantineStrategy& strategy);

    /**
     * Disable Byzantine behavior on node
     */
    void disableByzantine(const NodeID& node_id);

    // ========================================================================
    // Trace Access
    // ========================================================================

    /**
     * Get execution trace (for oracle analysis)
     */
    ConsensusTrace getTrace() const;

    /**
     * Capture state snapshots for all nodes
     *
     * Called automatically by tick() at snapshot_interval_.
     */
    void captureSnapshots();

    /**
     * Set snapshot interval (0 = disabled)
     */
    void setSnapshotInterval(uint64_t interval_ms) {
        snapshot_interval_ms_ = interval_ms;
    }

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get total events recorded
     */
    size_t getTotalEventCount() const;

    /**
     * Get total snapshots captured
     */
    size_t getTotalSnapshotCount() const;

    /**
     * Get network statistics
     */
    struct NetworkStats {
        uint64_t messages_sent;
        uint64_t messages_delivered;
        uint64_t messages_dropped;
        uint64_t messages_blocked;
        uint64_t pending_messages;
    };
    NetworkStats getNetworkStats() const;

private:
    // Configuration
    NetworkTopology topology_;
    dinero::ChainParams params_;
    uint64_t rng_seed_;
    std::string scenario_name_;

    // Simulation state
    bool running_;
    uint64_t current_time_;
    uint64_t action_sequence_;

    // Nodes
    std::map<NodeID, std::unique_ptr<ConsensusNode>> nodes_;

    // Network
    std::unique_ptr<NetworkSimulator> network_;

    // Trace recording
    std::vector<ConsensusAction> actions_;
    std::vector<ConsensusEvent> events_;
    std::vector<ConsensusState> snapshots_;

    // Snapshot control
    uint64_t snapshot_interval_ms_;
    uint64_t last_snapshot_time_;

    // Helper: Record action
    void recordAction(
        ConsensusActionType type,
        const std::optional<NodeID>& node_id = std::nullopt,
        const std::optional<std::string>& payload = std::nullopt
    );

    // Helper: Collect all events from all nodes
    void collectEvents();

    // Helper: Establish connections per topology
    void establishConnections();
};

} // namespace test
} // namespace consensus
} // namespace dinero
