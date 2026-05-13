#pragma once

#include "../framework/consensus_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace consensus {
namespace test {

/**
 * ByzantineViolation - Byzantine tolerance property violation
 *
 * Describes when Byzantine (malicious) node behavior causes undesirable outcomes
 */
struct ByzantineViolation {
    std::string property_name;  // e.g., "DB1: Network Resilience"
    std::string description;    // Human-readable violation description
    uint64_t timestamp;         // When violation was detected
    std::vector<std::string> involved_nodes;  // Which nodes were affected
    std::string details;        // Additional diagnostic information

    ByzantineViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t time = 0
    ) : property_name(prop_name)
      , description(desc)
      , timestamp(time)
    {}
};

/**
 * ByzantineToleranceOracle - Base class for Byzantine tolerance oracles
 *
 * Byzantine tolerance properties answer: "Does the network handle malicious nodes correctly?"
 * - DB1: Network Resilience - Network makes progress despite Byzantine nodes
 * - DB2: Eclipse Resistance - Honest nodes with honest peers converge correctly
 * - DB3: Double-Spend Resistance - Conflicting transactions don't both confirm
 * - DB4: Block Withholding Tolerance - Chain grows despite block withholding
 * - DB5: Invalid Block Rejection - Honest nodes reject invalid blocks
 *
 * Pattern (following Ring 4's oracle pattern):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for Byzantine violations
 * 3. check(trace) - Public API returning violations
 *
 * Key Principles (Observable Facts Only):
 * - Check outcomes, not intent
 * - Byzantine nodes are marked is_byzantine=true in trace
 * - Honest nodes are marked is_byzantine=false
 * - No inference about "should" or "must" - only "did"
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Check only observable trace facts
 * - Return empty vector if property holds, populated vector if violations found
 */
class ByzantineToleranceOracle {
public:
    virtual ~ByzantineToleranceOracle() = default;

    /**
     * Check trace for Byzantine tolerance violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<ByzantineViolation> check(const ConsensusTrace& trace) {
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
     * Observe trace and detect Byzantine violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<ByzantineViolation> observeTrace(const ConsensusTrace& trace) = 0;

    // ========================================================================
    // Helper Methods for Subclasses
    // ========================================================================

    /**
     * Get all honest nodes from trace (is_byzantine=false)
     */
    std::vector<NodeID> getHonestNodes(const ConsensusTrace& trace) const;

    /**
     * Get all Byzantine nodes from trace (is_byzantine=true)
     */
    std::vector<NodeID> getByzantineNodes(const ConsensusTrace& trace) const;

    /**
     * Get final state for node at end of trace
     */
    std::optional<ConsensusState> getFinalState(const ConsensusTrace& trace, const NodeID& node_id) const;

    /**
     * Check if all honest nodes have converged to same chain tip
     */
    bool haveHonestNodesConverged(const ConsensusTrace& trace) const;

    /**
     * Get events of a specific type
     */
    std::vector<ConsensusEvent> getEventsOfType(
        const ConsensusTrace& trace,
        ConsensusEventType type
    ) const;

    /**
     * Get events for a specific node
     */
    std::vector<ConsensusEvent> getEventsForNode(
        const ConsensusTrace& trace,
        const NodeID& node_id
    ) const;

    /**
     * Check if network made progress (chain height increased)
     */
    bool didNetworkMakeProgress(
        const ConsensusTrace& trace,
        uint64_t start_time,
        uint64_t end_time
    ) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
