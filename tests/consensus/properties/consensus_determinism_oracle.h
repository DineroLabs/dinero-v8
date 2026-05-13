#pragma once

#include "../framework/consensus_trace.h"
#include <string>
#include <vector>

namespace dinero {
namespace consensus {
namespace test {

/**
 * DeterminismViolation - Determinism property violation
 *
 * Describes when simulator execution is non-deterministic
 */
struct DeterminismViolation {
    std::string property_name;  // e.g., "DD1: Trace Reproducibility"
    std::string description;    // Human-readable violation description
    uint64_t seed_used;         // RNG seed that caused violation
    std::string details;        // Additional diagnostic information

    DeterminismViolation(
        const std::string& prop_name,
        const std::string& desc,
        uint64_t seed = 0
    ) : property_name(prop_name)
      , description(desc)
      , seed_used(seed)
    {}
};

/**
 * ConsensusDeterminismOracle - Base class for determinism oracles
 *
 * Determinism properties answer: "Is the simulator fully deterministic?"
 * - DD1: Trace Reproducibility - Same seed → same trace hash
 * - DD2: Message Delivery Determinism - Same schedule → same delivery order
 * - DD3: State Convergence Determinism - Same actions → same final state
 * - DD4: Reorg Determinism - Same fork → same resolution
 * - DD5: Byzantine Determinism - Same seed → same Byzantine behavior
 *
 * Pattern (following Ring 4's oracle pattern):
 * 1. reset() - Clear state before new trace
 * 2. observe(traces) - Analyze multiple traces for non-determinism
 * 3. check(traces) - Public API returning violations
 *
 * Key Principles (Observable Facts Only):
 * - Check trace equality, not execution intent
 * - Compare hashes, sequences, and states
 * - No inference about "should" - only "did match"
 *
 * Subclass Responsibilities:
 * - Implement observeTraces() to detect property-specific violations
 * - Check only observable trace facts
 * - Return empty vector if property holds, populated vector if violations found
 */
class ConsensusDeterminismOracle {
public:
    virtual ~ConsensusDeterminismOracle() = default;

    /**
     * Check traces for determinism violations
     *
     * @param traces Multiple execution traces (typically same scenario with same seed)
     * @return List of violations (empty if property holds)
     */
    std::vector<DeterminismViolation> check(const std::vector<ConsensusTrace>& traces) {
        reset();
        return observeTraces(traces);
    }

    /**
     * Get oracle name (for reporting)
     */
    virtual std::string getName() const = 0;

protected:
    /**
     * Reset oracle state before analyzing new traces
     *
     * Subclasses override to clear property-specific state
     */
    virtual void reset() {
        // Default: no state to clear
    }

    /**
     * Observe traces and detect determinism violations
     *
     * Subclasses implement property-specific detection logic
     *
     * @param traces Execution traces to analyze
     * @return List of violations (empty if property holds)
     */
    virtual std::vector<DeterminismViolation> observeTraces(
        const std::vector<ConsensusTrace>& traces
    ) = 0;

    // ========================================================================
    // Helper Methods for Subclasses
    // ========================================================================

    /**
     * Check if all traces have same final hash
     */
    bool tracesHaveSameHash(const std::vector<ConsensusTrace>& traces) const;

    /**
     * Check if all traces have same number of events
     */
    bool tracesHaveSameEventCount(const std::vector<ConsensusTrace>& traces) const;

    /**
     * Check if all traces have same event sequence
     */
    bool tracesHaveSameEventSequence(const std::vector<ConsensusTrace>& traces) const;

    /**
     * Check if all traces have same final state for all nodes
     */
    bool tracesHaveSameFinalState(const std::vector<ConsensusTrace>& traces) const;

    /**
     * Get final state for a specific node in a trace
     */
    std::optional<ConsensusState> getFinalStateForNode(
        const ConsensusTrace& trace,
        const NodeID& node_id
    ) const;

    /**
     * Compare two events for equality (type, timestamp, sequence, node_id)
     */
    bool eventsEqual(const ConsensusEvent& e1, const ConsensusEvent& e2) const;

    /**
     * Compare two states for equality (chain_tip, height, chainwork)
     */
    bool statesEqual(const ConsensusState& s1, const ConsensusState& s2) const;
};

} // namespace test
} // namespace consensus
} // namespace dinero
