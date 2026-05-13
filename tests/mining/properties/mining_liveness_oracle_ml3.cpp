#include "mining_liveness_oracle_ml3.h"
#include <sstream>

// Ring 4 Phase 4e: ML3 - Blocks Eventually Submitted Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

ML3Oracle::ML3Oracle(const ConsensusParams& params)
    : MiningLivenessOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string ML3Oracle::name() const {
    return "ML3: Blocks Eventually Submitted";
}

void ML3Oracle::reset() {
    this->MiningLivenessOracle::reset();

    solution_pending_ = false;
    events_since_solution_ = 0;
    solution_event_index_ = 0;
    total_solutions_ = 0;
    total_submissions_ = 0;
}

// ============================================================================
// Event Observation
// ============================================================================

void ML3Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Count events while solution is pending
    if (solution_pending_) {
        events_since_solution_++;

        // Check if threshold exceeded without submission
        if (events_since_solution_ > kSubmissionThreshold) {
            std::ostringstream msg;
            msg << "Solution found at event " << solution_event_index_
                << " but not submitted after " << events_since_solution_
                << " events (threshold: " << kSubmissionThreshold << ")";

            this->reportViolation("ML3", msg.str(), event_index);

            // Reset to avoid repeated violations
            solution_pending_ = false;
        }
    }

    // Track SOLUTION_FOUND events
    if (event.type == MiningEventType::SOLUTION_FOUND) {
        solution_pending_ = true;
        events_since_solution_ = 0;
        solution_event_index_ = event_index;
        total_solutions_++;
    }

    // Track BLOCK_SUBMITTED events
    if (event.type == MiningEventType::BLOCK_SUBMITTED) {
        total_submissions_++;

        // Submission occurred - forward progress made
        if (solution_pending_) {
            solution_pending_ = false;
        }
    }

    // Track BLOCK_REJECTED - also clears pending state
    // (solution was submitted but rejected by consensus)
    if (event.type == MiningEventType::BLOCK_REJECTED) {
        // Block was submitted (even if rejected)
        // This counts as forward progress for ML3
        if (solution_pending_) {
            solution_pending_ = false;
        }
    }

    // Track POW_STOPPED - mining stopped
    // If solution is pending when mining stops, that might indicate a problem
    if (event.type == MiningEventType::POW_STOPPED) {
        if (solution_pending_) {
            // Mining stopped with pending solution
            // This might be OK (user stopped) or might indicate stuck submission
            // Only flag if significant events passed
            if (events_since_solution_ > kSubmissionThreshold) {
                std::ostringstream msg;
                msg << "Mining stopped with pending solution (found at event "
                    << solution_event_index_ << ") after "
                    << events_since_solution_ << " events without submission";

                this->reportViolation("ML3", msg.str(), event_index);
            }

            solution_pending_ = false;
        }
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void ML3Oracle::finalize() {
    // Check if trace ended with pending solution
    if (solution_pending_) {
        if (events_since_solution_ > kSubmissionThreshold) {
            std::ostringstream msg;
            msg << "Trace ended with pending solution (found at event "
                << solution_event_index_ << ") after "
                << events_since_solution_ << " events without submission";

            this->reportViolation("ML3", msg.str(), 0);
        }
    }
}

}  // namespace mining_test
