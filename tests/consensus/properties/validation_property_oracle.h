#pragma once

#include "validation_trace.h"
#include <vector>
#include <string>
#include <cstdint>

// Ring 2 Phase 4: Validation Property Oracle (Base Class)
// Purpose: Stateful oracle that detects validation invariant violations
// Pattern: Follows Ring 4 oracle design (reset → observe → finalize)

namespace dinero::consensus::test {

// ============================================================================
// ValidationViolation - Represents a detected property violation
// ============================================================================

struct ValidationViolation {
    std::string property;      // e.g. "V4.1", "V4.2", etc.
    std::string message;       // Human-readable explanation
    uint64_t at_event{0};      // Index into ValidationTrace.events

    ValidationViolation() = default;

    ValidationViolation(const std::string& prop, const std::string& msg, uint64_t event_idx)
        : property(prop), message(msg), at_event(event_idx) {}
};

// ============================================================================
// ValidationPropertyOracle - Base class for validation property checkers
// ============================================================================

/**
 * ValidationPropertyOracle
 *
 * Stateful oracle that detects validation property violations
 * across the full validation trace.
 *
 * Design:
 * - reset() called before each trace
 * - observe() called for each event in sequence
 * - finalize() called after all events processed
 * - Violations accumulated and returned via check()
 *
 * Pattern identical to Ring 4's MiningSafetyOracle.
 */
class ValidationPropertyOracle {
public:
    ValidationPropertyOracle() = default;
    virtual ~ValidationPropertyOracle() = default;

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
     * @param state Current validation state snapshot
     * @param event Current event being processed
     * @param event_index Index of event in trace
     */
    virtual void observe(
        const ValidationState& state,
        const ValidationEvent& event,
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
    std::vector<ValidationViolation> check(const ValidationTrace& trace);

protected:
    /**
     * Record a validation violation.
     */
    void reportViolation(
        const std::string& property,
        const std::string& message,
        uint64_t event_index
    );

private:
    std::vector<ValidationViolation> violations_;
};

} // namespace dinero::consensus::test
