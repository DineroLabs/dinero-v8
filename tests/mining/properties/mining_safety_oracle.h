#pragma once

#include "consensus_params.h"
#include "subsidy_calculator.h"
#include "../framework/mining_trace.h"
#include "../framework/mining_types.h"
#include <vector>
#include <string>
#include <cstdint>

// Ring 4 Phase 4d: Mining Safety Oracle (Base Class)
// Purpose: Stateful oracle that detects safety violations across mining traces
// Rule: Safety = "nothing bad ever happens"

namespace mining_test {

// ============================================================================
// SafetyViolation - Represents a detected safety invariant violation
// ============================================================================

struct SafetyViolation {
    std::string property;      // e.g. "MS1", "MS2", etc.
    std::string message;       // Human-readable explanation
    uint64_t at_event{0};      // Index into MiningTrace.events

    SafetyViolation() = default;

    SafetyViolation(const std::string& prop, const std::string& msg, uint64_t event_idx)
        : property(prop), message(msg), at_event(event_idx) {}
};

// ============================================================================
// MiningSafetyOracle - Base class for safety property checkers
// ============================================================================

/**
 * MiningSafetyOracle
 *
 * Stateful oracle that detects safety violations across
 * the full mining trace.
 *
 * Safety = "nothing bad ever happens".
 *
 * Design:
 * - Observe() called for each event in sequence
 * - Implementations update internal state
 * - Finalize() called after all events processed
 * - Violations accumulated and returned
 */
class MiningSafetyOracle {
public:
    // Constructor with consensus parameters
    explicit MiningSafetyOracle(const ConsensusParams& params);

    virtual ~MiningSafetyOracle() = default;

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
    std::vector<SafetyViolation> check(const MiningTrace& trace);

    // Get consensus params
    const ConsensusParams& getParams() const { return params_; }

    // Get subsidy calculator
    const ConsensusSubsidyCalculator& getSubsidyCalculator() const { return subsidy_calc_; }

protected:
    /**
     * Record a safety violation.
     */
    void reportViolation(
        const std::string& property,
        const std::string& message,
        uint64_t event_index
    );

    // Consensus parameters
    ConsensusParams params_;

    // Subsidy calculator (for MS1/MS2)
    ConsensusSubsidyCalculator subsidy_calc_;

private:
    std::vector<SafetyViolation> violations_;
};

}  // namespace mining_test
