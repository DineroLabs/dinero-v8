#pragma once

#include "consensus_types.h"
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace dinero {
namespace consensus {
namespace test {

// Explicit selected-chain evidence for the DC3/DC4 oracles. TX_ACCEPTED is
// only a mempool event, and BLOCK_ACCEPTED alone does not identify inputs or
// a block's parent. Producers must supply this evidence before claiming that
// either property was checked.
struct FinalizedSpend {
    std::string tx_id;
    std::string outpoint;
};

struct FinalizedBlockObservation {
    NodeID node_id;
    uint32_t height;
    std::string block_hash;
    std::string parent_hash;
    std::vector<FinalizedSpend> spends;
};

/**
 * ConsensusTrace - Complete execution history for multi-node consensus simulation
 *
 * Analogous to MiningTrace from Ring 4, but extends to multi-node scenarios.
 * Records all actions (inputs), events (outputs), and state snapshots across
 * all nodes in the network.
 *
 * Pattern: Simulator executes actions → generates events → captures state snapshots
 * Oracle: Reads trace → observes events → reports violations
 */
struct ConsensusTrace {
    // ========================================================================
    // Configuration
    // ========================================================================

    uint64_t rng_seed;              // For deterministic reproducibility
    std::string scenario_name;       // Test identification (e.g., "2-way partition")
    NetworkTopology topology;        // Network connectivity graph

    // ========================================================================
    // Execution History (Global Timeline)
    // ========================================================================

    // Actions: What we commanded the system to do
    std::vector<ConsensusAction> actions;

    // Events: What actually happened (per-node outputs)
    std::vector<ConsensusEvent> events;

    // State snapshots: Periodic checkpoints of all node states
    std::vector<ConsensusState> snapshots;

    // Selected finalized history, when the producer can actually observe it.
    // The Phase-5b simulator does not currently populate this field.
    std::vector<FinalizedBlockObservation> finalized_blocks;

    // ========================================================================
    // Determinism Verification
    // ========================================================================

    uint64_t final_hash;             // Hash of entire trace (for DD1 oracle)

    // ========================================================================
    // Metadata
    // ========================================================================

    uint64_t start_time;             // Simulation start timestamp
    uint64_t end_time;               // Simulation end timestamp
    bool completed_successfully;     // Did simulation complete without errors?
    std::optional<std::string> failure_reason;  // Error details if failed

    // ========================================================================
    // Methods
    // ========================================================================

    /**
     * Compute deterministic hash of entire trace
     *
     * Hash includes:
     * - RNG seed
     * - All actions (type, timestamp, sequence, parameters)
     * - All events (type, timestamp, sequence, node_id, success)
     * - All state snapshots (node_id, timestamp, chain_tip, height, chainwork)
     * - Supplied finalized selected blocks and their spend evidence
     *
     * Used by DD1 oracle to verify trace reproducibility.
     */
    uint64_t computeHash() const;

    /**
     * Get all events for a specific node
     */
    std::vector<ConsensusEvent> getEventsForNode(const NodeID& node_id) const;

    /**
     * Get all state snapshots for a specific node
     */
    std::vector<ConsensusState> getSnapshotsForNode(const NodeID& node_id) const;

    /**
     * Get state snapshot at specific timestamp (closest snapshot before timestamp)
     */
    std::optional<ConsensusState> getStateAt(const NodeID& node_id, uint64_t timestamp) const;

    /**
     * Get all nodes that participated in this trace
     */
    std::vector<NodeID> getAllNodes() const;

    /**
     * Get simulation duration (end_time - start_time)
     */
    uint64_t getDuration() const {
        return end_time - start_time;
    }

    /**
     * Get total number of events across all nodes
     */
    size_t getTotalEventCount() const {
        return events.size();
    }

    /**
     * Get total number of snapshots across all nodes
     */
    size_t getTotalSnapshotCount() const {
        return snapshots.size();
    }
};

} // namespace test
} // namespace consensus
} // namespace dinero
