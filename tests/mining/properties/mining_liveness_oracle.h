#pragma once

#include "consensus_params.h"
#include "../framework/mining_trace.h"
#include "../framework/mining_types.h"
#include <vector>
#include <string>
#include <cstdint>

// Ring 4 Phase 4e: Mining Liveness Oracle (Base Class)
// Purpose: Stateful oracle that detects liveness violations across mining traces
// Rule: Liveness = "something good eventually happens"

namespace mining_test {

// ============================================================================
// LivenessViolation - Represents a detected liveness violation
// ============================================================================

struct LivenessViolation {
    std::string property;      // e.g. "ML1", "ML2", etc.
    std::string message;       // Human-readable explanation
    uint64_t at_event{0};      // Index into MiningTrace.events

    LivenessViolation() = default;

    LivenessViolation(const std::string& prop, const std::string& msg, uint64_t event_idx)
        : property(prop), message(msg), at_event(event_idx) {}
};

// ============================================================================
// MiningLivenessOracle - Base class for liveness property checkers
// ============================================================================

/**
 * MiningLivenessOracle
 *
 * Stateful oracle that detects liveness violations across
 * the full mining trace.
 *
 * Liveness = "something good eventually happens".
 *
 * Design:
 * - Observe() called for each event in sequence
 * - Implementations update internal state
 * - Finalize() called after all events processed
 * - Violations accumulated and returned
 */
class MiningLivenessOracle {
public:
    // Constructor with consensus parameters
    explicit MiningLivenessOracle(const ConsensusParams& params);

    virtual ~MiningLivenessOracle() = default;

    /**
     * Human-readable name of the oracle
     */
    virtual std::string name() const = 0;

    /**
     * Reset internal state.
     * Called before every trace evaluation.
     */
    virtual void reset();

    /**
     * Observe one event in sequence.
     * Implementations update internal state here.
     *
     * @param state Current mining state snapshot
     * @param event Current event being processed
     * @param event_index Index of event in trace
     */
    virtual void observe(
        const MiningState& state,
        const MiningEvent& event,
        uint64_t event_index
    ) = 0;

    /**
     * Final evaluation after all events processed.
     * Allows detection of end-of-trace violations.
     */
    virtual void finalize();

    /**
     * Run the oracle against a full trace.
     * Returns list of detected violations.
     */
    std::vector<LivenessViolation> check(const MiningTrace& trace);

    // Get consensus params
    const ConsensusParams& getParams() const { return params_; }

protected:
    /**
     * Record a liveness violation.
     */
    void reportViolation(
        const std::string& property,
        const std::string& message,
        uint64_t event_index
    );

    // Consensus parameters
    ConsensusParams params_;

private:
    std::vector<LivenessViolation> violations_;
};

}  // namespace mining_test
