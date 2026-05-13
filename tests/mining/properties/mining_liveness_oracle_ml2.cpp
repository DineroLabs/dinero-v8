#include "mining_liveness_oracle_ml2.h"
#include <sstream>

// Ring 4 Phase 4e: ML2 - Solutions Eventually Found Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

ML2Oracle::ML2Oracle(const ConsensusParams& params)
    : MiningLivenessOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string ML2Oracle::name() const {
    return "ML2: Solutions Eventually Found";
}

void ML2Oracle::reset() {
    this->MiningLivenessOracle::reset();

    mining_active_ = false;
    events_since_mining_start_ = 0;
    mining_start_event_index_ = 0;
    solution_found_ = false;
    total_solutions_ = 0;
}

// ============================================================================
// Event Observation
// ============================================================================

void ML2Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track POW_STARTED - mining becomes active
    if (event.type == MiningEventType::POW_STARTED) {
        mining_active_ = true;
        events_since_mining_start_ = 0;
        mining_start_event_index_ = event_index;
        solution_found_ = false;
    }

    // Count events while mining is active
    if (mining_active_) {
        events_since_mining_start_++;

        // Check if threshold exceeded without solution
        if (!solution_found_ && events_since_mining_start_ > kSolutionFindingThreshold) {
            std::ostringstream msg;
            msg << "Mining active since event " << mining_start_event_index_
                << " but no solution found after " << events_since_mining_start_
                << " events (threshold: " << kSolutionFindingThreshold << ")";

            this->reportViolation("ML2", msg.str(), event_index);

            // Reset to avoid repeated violations
            mining_active_ = false;
        }
    }

    // Track SOLUTION_FOUND events
    if (event.type == MiningEventType::SOLUTION_FOUND) {
        solution_found_ = true;
        total_solutions_++;

        // Solution found - forward progress made
        // Reset mining period tracking
        mining_active_ = false;
    }

    // Track POW_STOPPED - mining stopped
    if (event.type == MiningEventType::POW_STOPPED) {
        // Mining stopped
        // If we didn't find a solution, that's OK in general
        // (user might stop mining before finding a solution)
        // Only flag if mining ran for extended period
        if (mining_active_ && !solution_found_) {
            if (events_since_mining_start_ > kSolutionFindingThreshold) {
                std::ostringstream msg;
                msg << "Mining ran for " << events_since_mining_start_
                    << " events without finding solution before stopping"
                    << " (started at event " << mining_start_event_index_ << ")";

                this->reportViolation("ML2", msg.str(), event_index);
            }
        }

        mining_active_ = false;
    }

    // Also consider TEMPLATE_DISCARDED as potential mining restart
    // (new block arrived, old work discarded, new mining cycle begins)
    if (event.type == MiningEventType::TEMPLATE_DISCARDED) {
        // If we were mining and template was discarded,
        // this starts a new mining period (new template will be created)
        if (mining_active_ && !solution_found_) {
            // Don't flag violation - reorg/new block is normal
            // Just reset the period tracking
            mining_active_ = false;
        }
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void ML2Oracle::finalize() {
    // Check if trace ended while mining without solution
    if (mining_active_ && !solution_found_) {
        if (events_since_mining_start_ > kSolutionFindingThreshold) {
            std::ostringstream msg;
            msg << "Trace ended while mining (started at event "
                << mining_start_event_index_ << ") without finding solution after "
                << events_since_mining_start_ << " events";

            this->reportViolation("ML2", msg.str(), 0);
        }
    }
}

}  // namespace mining_test
