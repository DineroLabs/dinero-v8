#pragma once

#include "../framework/consensus_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace consensus {
namespace test {

/**
 * Violation - Safety property violation record
 *
 * Describes what went wrong and where
 */
struct Violation {
    std::string property_name;  // e.g., "DC1: Agreement"
    std::string description;    // Human-readable violation description
    uint64_t timestamp;         // When violation occurred
    std::vector<std::string> involved_nodes;  // Which nodes were involved
    std::string details;        // Additional diagnostic information

    Violation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t ts = 0
    ) : property_name(prop_name)
      , description(desc)
      , timestamp(ts)
    {}
};

/**
 * ConsensusSafetyOracle - Base class for safety property oracles
 *
 * Safety properties answer: "Nothing bad happens"
 * - DC1: Agreement - Honest nodes agree on same block at each height
 * - DC2: Validity - Only valid blocks accepted
 * - DC3: Integrity - No double-spend survives
 * - DC4: Total Ordering - Consistent block sequence
 * - DC5: Finality - Blocks beyond threshold never revert
 *
 * Pattern (following Ring 4's MiningSafetyOracle):
 * 1. reset() - Clear state before new trace
 * 2. observe(trace) - Analyze trace for violations
 * 3. check(trace) - Public API returning violations
 *
 * Subclass Responsibilities:
 * - Implement observeTrace() to detect property-specific violations
 * - Return empty vector if no violations, populated vector if violations found
 */
class ConsensusSafetyOracle {
public:
    virtual ~ConsensusSafetyOracle() = default;

    /**
     * Check trace for violations
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    std::vector<Violation> check(const ConsensusTrace& trace) {
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
     * Observe trace and detect violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param trace Execution trace to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<Violation> observeTrace(const ConsensusTrace& trace) = 0;

    // ========================================================================
    // Helper Methods for Subclasses
    // ========================================================================

    /**
     * Get all honest nodes from trace (non-Byzantine)
     */
    std::vector<NodeID> getHonestNodes(const ConsensusTrace& trace) const;

    /**
     * Get all Byzantine nodes from trace
     */
    std::vector<NodeID> getByzantineNodes(const ConsensusTrace& trace) const;

    /**
     * Get final state for node at end of trace
     */
    std::optional<ConsensusState> getFinalState(const ConsensusTrace& trace, const NodeID& node_id) const;

    /**
     * Get all BLOCK_ACCEPTED events for a node
     */
    std::vector<ConsensusEvent> getBlockAcceptedEvents(const ConsensusTrace& trace, const NodeID& node_id) const;

    /**
     * Get all CHAIN_TIP_CHANGED events for a node
     */
    std::vector<ConsensusEvent> getChainTipChangedEvents(const ConsensusTrace& trace, const NodeID& node_id) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
