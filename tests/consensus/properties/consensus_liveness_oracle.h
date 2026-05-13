#pragma once

#include "../framework/consensus_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace consensus {
namespace test {

/**
 * LivenessViolation - Liveness property violation record
 *
 * Describes when expected progress didn't happen within timeout
 */
struct LivenessViolation {
    std::string property_name;  // e.g., "DL1: Eventual Consensus"
    std::string description;    // Human-readable violation description
    uint64_t expected_by;       // When we expected progress by
    uint64_t actual_time;       // When (if ever) it actually happened
    std::vector<std::string> involved_nodes;  // Which nodes failed to make progress
    std::string details;        // Additional diagnostic information

    LivenessViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t expected = 0,
        uint64_t actual = 0
    ) : property_name(prop_name)
      , description(desc)
      , expected_by(expected)
      , actual_time(actual)
    {}
};

/**
 * ConsensusLivenessOracle - Base class for liveness property oracles
 *
 * Liveness properties answer: "Something good eventually happens"
 * - DL1: Eventual Consensus - Nodes converge after partition heals
 * - DL2: Block Propagation - Blocks reach all nodes within timeout
 * - DL3: Chain Growth - Chain height increases monotonically
 * - DL4: Transaction Inclusion - Valid txs eventually included in blocks
 * - DL5: Sync Completion - New nodes reach tip within bounded time
 *
 * Pattern (following Ring 4's MiningLivenessOracle):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for liveness violations
 * 3. check(trace) - Public API returning violations
 *
 * Key Difference from Safety:
 * - Safety: "Bad thing never happens" (invariant violation)
 * - Liveness: "Good thing eventually happens" (progress within timeout)
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Define reasonable timeouts for "eventually"
 * - Return empty vector if property holds, populated vector if violations found
 */
class ConsensusLivenessOracle {
public:
    virtual ~ConsensusLivenessOracle() = default;

    /**
     * Check trace for liveness violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<LivenessViolation> check(const ConsensusTrace& trace) {
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
     * Observe trace and detect liveness violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<LivenessViolation> observeTrace(const ConsensusTrace& trace) = 0;

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
     * Get all events for a specific node
     */
    std::vector<ConsensusEvent> getEventsForNode(
        const ConsensusTrace& trace,
        const NodeID& node_id
    ) const;

    /**
     * Get time when partition healed (if it did)
     */
    std::optional<uint64_t> getPartitionHealTime(const ConsensusTrace& trace) const;

    /**
     * Get time when network partition occurred
     */
    std::optional<uint64_t> getPartitionStartTime(const ConsensusTrace& trace) const;

    /**
     * Check if all honest nodes have converged to same chain tip
     */
    bool haveNodesConverged(const ConsensusTrace& trace) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
