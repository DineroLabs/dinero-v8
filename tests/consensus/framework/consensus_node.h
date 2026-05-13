#pragma once

#include "consensus_types.h"
#include "consensus_trace.h"
#include <memory>
#include <vector>
#include <optional>
#include <map>

namespace dinero {
namespace consensus {
namespace test {

/**
 * ConsensusNode - Single node in multi-node consensus simulation
 *
 * Wraps a MiningSimulator (Ring 4) and adds:
 * - P2P connectivity to other nodes
 * - Message queue for network simulation
 * - Byzantine behavior injection
 * - State snapshot capture
 *
 * Design Pattern:
 * - Each node maintains independent chain state (ChainState from Ring 1/2)
 * - Each node has independent MiningSimulator (Ring 4)
 * - NetworkSimulator routes messages between nodes
 * - ConsensusSimulator orchestrates all nodes
 */
class ConsensusNode {
public:
    /**
     * Create a consensus node
     *
     * @param node_id Unique node identifier (e.g., "alice", "bob")
     * @param params Blockchain parameters (regtest/mainnet)
     * @param rng_seed Deterministic RNG seed
     */
    ConsensusNode(
        const NodeID& node_id,
        const dinero::ChainParams& params,
        uint64_t rng_seed
    );

    ~ConsensusNode();

    // ========================================================================
    // Node Lifecycle
    // ========================================================================

    /**
     * Start node (initialize chain state, connect to network)
     */
    void start();

    /**
     * Stop node (disconnect from network, halt mining)
     */
    void stop();

    /**
     * Check if node is running
     */
    bool isRunning() const { return running_; }

    // ========================================================================
    // P2P Connectivity
    // ========================================================================

    /**
     * Connect to peer node
     */
    void connectToPeer(const NodeID& peer_id);

    /**
     * Disconnect from peer node
     */
    void disconnectFromPeer(const NodeID& peer_id);

    /**
     * Check if connected to peer
     */
    bool isConnectedTo(const NodeID& peer_id) const;

    /**
     * Get all connected peers
     */
    std::vector<NodeID> getConnectedPeers() const;

    // ========================================================================
    // Mining Control
    // ========================================================================

    /**
     * Start mining blocks
     */
    void startMining();

    /**
     * Stop mining blocks
     */
    void stopMining();

    /**
     * Check if node is mining
     */
    bool isMining() const;

    /**
     * Get current mining phase
     */
    MiningPhase getMiningPhase() const;

    // ========================================================================
    // Blockchain Operations
    // ========================================================================

    /**
     * Receive block from peer
     *
     * @param block_hash Block identifier
     * @param from_peer Peer that sent the block
     * @return true if block accepted, false if rejected
     */
    bool receiveBlock(const std::string& block_hash, const NodeID& from_peer);

    /**
     * Broadcast block to all connected peers
     *
     * @param block_hash Block to broadcast
     * @return List of peers block was sent to
     */
    std::vector<NodeID> broadcastBlock(const std::string& block_hash);

    /**
     * Receive transaction from peer
     */
    bool receiveTransaction(const std::string& tx_id, const NodeID& from_peer);

    /**
     * Broadcast transaction to all connected peers
     */
    std::vector<NodeID> broadcastTransaction(const std::string& tx_id);

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * Get current chain tip hash
     */
    std::string getChainTipHash() const;

    /**
     * Get current chain height
     */
    uint32_t getChainHeight() const;

    /**
     * Get current chainwork
     */
    uint64_t getChainwork() const;

    /**
     * Get mempool size
     */
    size_t getMempoolSize() const;

    /**
     * Get mempool transaction IDs
     */
    std::vector<std::string> getMempoolTxids() const;

    /**
     * Check if syncing
     */
    bool isSyncing() const { return is_syncing_; }

    /**
     * Capture current state snapshot (for ConsensusTrace)
     */
    ConsensusState captureState(uint64_t timestamp) const;

    // ========================================================================
    // Byzantine Behavior
    // ========================================================================

    /**
     * Enable Byzantine behavior
     */
    void enableByzantine(const ByzantineStrategy& strategy);

    /**
     * Disable Byzantine behavior (revert to honest)
     */
    void disableByzantine();

    /**
     * Check if node is Byzantine
     */
    bool isByzantine() const { return is_byzantine_; }

    /**
     * Get Byzantine strategy
     */
    std::optional<ByzantineStrategy> getByzantineStrategy() const;

    // ========================================================================
    // Message Queue (Network Simulation)
    // ========================================================================

    /**
     * Enqueue message from network
     *
     * Called by NetworkSimulator when message arrives
     */
    void enqueueMessage(
        const NodeID& from_peer,
        ConsensusEventType message_type,
        const std::string& payload,
        uint64_t timestamp
    );

    /**
     * Process pending messages
     *
     * Called by ConsensusSimulator each tick
     */
    void processMessages(uint64_t current_time);

    // ========================================================================
    // Event Recording (for ConsensusTrace)
    // ========================================================================

    /**
     * Record event to trace
     */
    void recordEvent(ConsensusEvent event);

    /**
     * Get all events recorded by this node
     */
    const std::vector<ConsensusEvent>& getEvents() const { return events_; }

    /**
     * Clear recorded events (for next simulation)
     */
    void clearEvents() { events_.clear(); }

    // ========================================================================
    // Getters
    // ========================================================================

    const NodeID& getNodeId() const { return node_id_; }
    const dinero::ChainParams& getParams() const { return params_; }

private:
    // Node identity
    NodeID node_id_;
    dinero::ChainParams params_;
    uint64_t rng_seed_;

    // Lifecycle state
    bool running_;

    // P2P connectivity
    std::vector<NodeID> connected_peers_;

    // Blockchain state (Ring 1/2)
    // Note: Simplified for simulation - real ChainState integration in Phase 5b
    std::string chain_tip_hash_;
    uint32_t chain_height_;
    uint64_t chainwork_;

    // Mempool state
    std::vector<std::string> mempool_txids_;

    // Mining state (Ring 4 integration will come in Phase 5b)
    bool is_mining_;

    // Sync state
    bool is_syncing_;
    std::optional<uint32_t> sync_target_height_;
    std::optional<NodeID> sync_peer_;

    // Byzantine state
    bool is_byzantine_;
    std::optional<ByzantineStrategy> byzantine_strategy_;

    // Message queue (from network)
    struct PendingMessage {
        NodeID from_peer;
        ConsensusEventType type;
        std::string payload;
        uint64_t timestamp;
    };
    std::vector<PendingMessage> message_queue_;

    // Event recording (for trace)
    std::vector<ConsensusEvent> events_;
    uint64_t event_sequence_;

    // Helper: Record event with automatic sequence numbering
    void recordEventInternal(
        ConsensusEventType type,
        uint64_t timestamp,
        bool success,
        const std::string& error_message = ""
    );
};

} // namespace test
} // namespace consensus
} // namespace dinero
