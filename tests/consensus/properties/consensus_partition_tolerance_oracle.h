#pragma once

#include "../framework/consensus_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace consensus {
namespace test {

/**
 * PartitionViolation - Network partition tolerance property violation
 *
 * Describes when network partitioning causes undesirable behavior
 */
struct PartitionViolation {
    std::string property_name;  // e.g., "DN1: Partition Tolerance"
    std::string description;    // Human-readable violation description
    uint64_t partition_time;    // When partition occurred
    uint64_t heal_time;         // When partition healed (if applicable)
    std::vector<std::string> involved_nodes;  // Which nodes were affected
    std::string details;        // Additional diagnostic information

    PartitionViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t partition = 0,
        uint64_t heal = 0
    ) : property_name(prop_name)
      , description(desc)
      , partition_time(partition)
      , heal_time(heal)
    {}
};

/**
 * PartitionToleranceOracle - Base class for partition tolerance oracles
 *
 * Partition tolerance properties answer: "Does the network handle partitions correctly?"
 * - DN1: Partition Tolerance - Majority partition makes progress
 * - DN2: Minority Stall - Minority produces only orphans
 * - DN3: Clean Healing - No block loss during partition healing
 * - DN4: Asynchronous Healing - Final state independent of healing order
 * - DN5: Cascading Partitions - Multiple sequential partitions converge
 *
 * Pattern (following Ring 4's oracle pattern):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for partition violations
 * 3. check(trace) - Public API returning violations
 *
 * Key Concepts:
 * - Majority partition: Group with >50% of nodes
 * - Minority partition: Group with ≤50% of nodes
 * - Orphan block: Block not on the main chain after healing
 * - Clean healing: All partitions merge without data loss
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Define what constitutes correct partition behavior
 * - Return empty vector if property holds, populated vector if violations found
 */
class PartitionToleranceOracle {
public:
    virtual ~PartitionToleranceOracle() = default;

    /**
     * Check trace for partition tolerance violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<PartitionViolation> check(const ConsensusTrace& trace) {
        reset();
        return observeTrace(trace);
    }

    /**
     * Get oracle name (for reporting)
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state before analyzing new trace
     *
     * Subclasses override to clear property-specific state
     */
    virtual void reset() {
        // Default: no state to clear
    }

    /**
     * Observe trace and detect partition violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<PartitionViolation> observeTrace(const ConsensusTrace& trace) = 0;

    // ========================================================================
    // Helper Methods for Subclasses
    // ========================================================================

    /**
     * Get all honest nodes from trace (non-Byzantine)
     */
    std::vector<NodeID> getHonestNodes(const ConsensusTrace& trace) const;

    /**
     * Get final state for node at end of trace
     */
    std::optional<ConsensusState> getFinalState(const ConsensusTrace& trace, const NodeID& node_id) const;

    /**
     * Get all events of a specific type
     */
    std::vector<ConsensusEvent> getEventsOfType(
        const ConsensusTrace& trace,
        ConsensusEventType type
    ) const;

    /**
     * Get time when partition was created
     */
    std::optional<uint64_t> getPartitionStartTime(const ConsensusTrace& trace) const;

    /**
     * Get time when partition healed
     */
    std::optional<uint64_t> getPartitionHealTime(const ConsensusTrace& trace) const;

    /**
     * Get nodes in partition groups (returns multiple groups)
     */
    std::vector<std::vector<NodeID>> getPartitionGroups(const ConsensusTrace& trace) const;

    /**
     * Check if node made progress (increased chain height)
     */
    bool didNodeMakeProgress(
        const ConsensusTrace& trace,
        const NodeID& node_id,
        uint64_t start_time,
        uint64_t end_time
    ) const;

    /**
     * Check if nodes converged to same chain tip
     */
    bool haveNodesConverged(
        const ConsensusTrace& trace,
        const std::vector<NodeID>& nodes
    ) const;

    /**
     * Get maximum chain height across nodes at specific time
     */
    uint32_t getMaxHeightAtTime(
        const ConsensusTrace& trace,
        const std::vector<NodeID>& nodes,
        uint64_t timestamp
    ) const;

    /**
     * Identify majority partition (>50% of honest nodes)
     */
    std::optional<std::vector<NodeID>> getMajorityPartition(
        const ConsensusTrace& trace,
        const std::vector<std::vector<NodeID>>& partitions
    ) const;

    /**
     * Identify minority partitions (≤50% of honest nodes)
     */
    std::vector<std::vector<NodeID>> getMinorityPartitions(
        const ConsensusTrace& trace,
        const std::vector<std::vector<NodeID>>& partitions
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
