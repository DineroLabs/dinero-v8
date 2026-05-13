#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <map>

// Forward declaration for ChainParams (full integration in Phase 5b)
namespace dinero {
    struct ChainParams {
        static ChainParams regtest() { return ChainParams(); }
    };
}

namespace dinero {
namespace consensus {
namespace test {

// Forward declarations
class Block;
class Transaction;

// ============================================================================
// Node Identification
// ============================================================================

using NodeID = std::string;

// ============================================================================
// Action Types (Inputs to the simulator - what we command)
// ============================================================================

enum class ConsensusActionType {
    // Network actions
    NODE_START,              // Node joins network
    NODE_STOP,               // Node leaves network
    PARTITION_NETWORK,       // Split network into groups
    HEAL_PARTITION,          // Rejoin partitions
    SET_LATENCY,             // Adjust network delay
    SET_PACKET_LOSS,         // Adjust drop rate

    // Mining actions (per node)
    START_MINING,            // Node begins mining
    STOP_MINING,             // Node stops mining
    BLOCK_MINED,             // Node finds block (deterministic PoW)

    // Blockchain actions
    BROADCAST_BLOCK,         // Node announces block
    REQUEST_BLOCK,           // Node requests block
    BROADCAST_TX,            // Node announces transaction
    REORG_DETECTED,          // Node switches chain

    // Byzantine actions
    ENABLE_BYZANTINE,        // Activate Byzantine behavior
    DISABLE_BYZANTINE,       // Revert to honest
    WITHHOLD_BLOCK,          // Selfish mining action
    DOUBLE_SPEND_ATTEMPT,    // Conflicting tx broadcast

    // Time actions
    ADVANCE_TIME             // Tick global clock
};

struct ConsensusAction {
    ConsensusActionType type;
    uint64_t timestamp;           // When this action occurs
    uint64_t sequence_number;     // Global action ordering

    // Action-specific data
    std::optional<NodeID> node_id;          // Which node (if applicable)
    std::optional<std::vector<NodeID>> node_group;  // Node groups for partitions
    std::optional<uint64_t> latency_ms;     // For SET_LATENCY
    std::optional<double> packet_loss_rate; // For SET_PACKET_LOSS
    std::optional<uint64_t> time_delta_ms;  // For ADVANCE_TIME
    std::optional<std::string> block_hash;  // For block-related actions
    std::optional<std::string> tx_id;       // For tx-related actions
    std::optional<std::string> byzantine_strategy;  // For Byzantine actions
};

// ============================================================================
// Event Types (Outputs from the simulator - what happened)
// ============================================================================

enum class ConsensusEventType {
    // Per-node events
    BLOCK_RECEIVED,          // Node received block
    BLOCK_VALIDATED,         // Block passed validation
    BLOCK_ACCEPTED,          // Block added to chain
    BLOCK_REJECTED,          // Block rejected
    CHAIN_TIP_CHANGED,       // Reorg occurred
    TX_RECEIVED,             // Transaction received
    TX_ACCEPTED,             // TX added to mempool
    TX_REJECTED,             // TX rejected
    PEER_CONNECTED,          // New peer connection
    PEER_DISCONNECTED,       // Peer dropped
    SYNC_STARTED,            // IBD/sync initiated
    SYNC_COMPLETED,          // Sync finished

    // Network events
    MESSAGE_SENT,            // Message queued
    MESSAGE_DELIVERED,       // Message arrived
    MESSAGE_DROPPED,         // Packet loss
    PARTITION_ACTIVATED,     // Network split
    PARTITION_HEALED,        // Network rejoined

    // Mining events
    MINING_STARTED,          // Node started mining
    MINING_STOPPED,          // Node stopped mining
    BLOCK_TEMPLATE_CREATED,  // Mining template created
    SOLUTION_FOUND           // PoW solution found
};

struct ConsensusEvent {
    ConsensusEventType type;
    uint64_t timestamp;           // When this event occurred
    uint64_t sequence_number;     // Global event ordering
    NodeID node_id;               // Which node generated this event

    // Event-specific data
    std::optional<std::string> block_hash;
    std::optional<std::string> tx_id;
    std::optional<uint32_t> block_height;
    std::optional<uint64_t> chainwork;
    std::optional<NodeID> peer_id;          // For peer events
    std::optional<std::string> message_type; // For message events

    bool success;                // Event outcome (e.g., validation result)
    std::string error_message;   // Error details (if success=false)
};

// ============================================================================
// State Types (System state snapshots)
// ============================================================================

enum class MiningPhase {
    STOPPED,
    IDLE,
    ASSEMBLING,
    MINING,
    SUBMITTING
};

struct ConsensusState {
    NodeID node_id;
    uint64_t timestamp;

    // Chain state (per node)
    std::string chain_tip_hash;
    uint32_t chain_height;
    uint64_t chainwork;

    // Mempool state
    std::vector<std::string> mempool_txids;
    size_t mempool_size_bytes;

    // Peer state
    std::vector<NodeID> connected_peers;
    size_t peer_count;

    // Mining state (from Ring 4)
    MiningPhase mining_phase;
    std::optional<std::string> template_hash;
    std::optional<uint32_t> template_height;

    // Sync state
    bool is_syncing;
    std::optional<uint32_t> sync_target_height;
    std::optional<NodeID> sync_peer;

    // Byzantine state
    bool is_byzantine;
    std::string byzantine_strategy;
};

// ============================================================================
// Network Topology
// ============================================================================

enum class TopologyType {
    FULL_MESH,      // All nodes connected to all others
    STAR,           // Hub-spoke topology
    CHAIN,          // Linear: n0→n1→n2→...
    CUSTOM          // User-defined connectivity
};

struct NetworkTopology {
    TopologyType type;
    std::vector<NodeID> nodes;

    // For CUSTOM topology: adjacency list
    // node_id → list of connected peer node_ids
    std::map<NodeID, std::vector<NodeID>> connections;

    // Helper: Create full mesh topology
    static NetworkTopology fullMesh(const std::vector<NodeID>& nodes);

    // Helper: Create star topology (nodes[0] is hub)
    static NetworkTopology star(const std::vector<NodeID>& nodes);

    // Helper: Create chain topology
    static NetworkTopology chain(const std::vector<NodeID>& nodes);
};

// ============================================================================
// Byzantine Strategies
// ============================================================================

enum class ByzantineStrategyType {
    HONEST,              // Not Byzantine (default)
    SELFISH_MINER,       // Withholds blocks strategically
    DOUBLE_SPENDER,      // Broadcasts conflicting txs
    ECLIPSE_ATTACKER,    // Feeds false blockchain
    BLOCK_WITHHOLDER,    // Mines but doesn't broadcast
    PROTOCOL_VIOLATOR,   // Invalid messages
    SLOW_PEER            // Delays all messages
};

struct ByzantineStrategy {
    ByzantineStrategyType type;

    // Strategy-specific parameters
    std::optional<double> withhold_probability;  // For selfish mining
    std::optional<uint32_t> delay_ms;            // For slow peer
    std::optional<std::vector<NodeID>> target_nodes;  // For eclipse
};

} // namespace test
} // namespace consensus
} // namespace dinero
