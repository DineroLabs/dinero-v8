#include "mining_liveness_oracle_ml4.h"
#include <sstream>

// Ring 4 Phase 4e: ML4 - Mining Eventually Restarts Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

ML4Oracle::ML4Oracle(const ConsensusParams& params)
    : MiningLivenessOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string ML4Oracle::name() const {
    return "ML4: Mining Eventually Restarts";
}

void ML4Oracle::reset() {
    this->MiningLivenessOracle::reset();

    system_restarted_ = false;
    mining_resumed_ = false;
    events_since_restart_ = 0;
    restart_event_index_ = 0;
    total_crashes_ = 0;
    total_restarts_ = 0;
    last_restart_count_ = 0;
}

// ============================================================================
// Event Observation
// ============================================================================

void ML4Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track state changes to detect crash/restart
    // state.has_crashed: true after crash, false after restart
    // state.restart_count: increments with each restart

    // Detect restart by checking restart_count increase
    if (state.restart_count > last_restart_count_) {
        // System restarted
        system_restarted_ = true;
        mining_resumed_ = false;
        events_since_restart_ = 0;
        restart_event_index_ = event_index;
        total_restarts_++;
        last_restart_count_ = state.restart_count;
    }

    // Count events while waiting for mining to resume after restart
    if (system_restarted_ && !mining_resumed_) {
        events_since_restart_++;

        // Check if threshold exceeded without mining resuming
        if (events_since_restart_ > kRestartThreshold) {
            std::ostringstream msg;
            msg << "System restarted at event " << restart_event_index_
                << " but mining not resumed after " << events_since_restart_
                << " events (threshold: " << kRestartThreshold << ")";

            this->reportViolation("ML4", msg.str(), event_index);

            // Reset to avoid repeated violations
            system_restarted_ = false;
        }
    }

    // Track POW_STARTED - mining resumed
    if (event.type == MiningEventType::POW_STARTED) {
        if (system_restarted_) {
            // Mining resumed after restart - forward progress made
            mining_resumed_ = true;
            system_restarted_ = false;
        }
    }

    // Track crashes (via ERROR_OCCURRED event or state.has_crashed flag)
    if (state.has_crashed) {
        // System is currently crashed
        // Count this for statistics, but don't flag violation yet
        // (violation is only if restart doesn't lead to mining resumption)
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void ML4Oracle::finalize() {
    // Check if trace ended waiting for mining to resume after restart
    if (system_restarted_ && !mining_resumed_) {
        if (events_since_restart_ > kRestartThreshold) {
            std::ostringstream msg;
            msg << "Trace ended after restart (at event "
                << restart_event_index_ << ") without mining resuming after "
                << events_since_restart_ << " events";

            this->reportViolation("ML4", msg.str(), 0);
        }
    }
}

}  // namespace mining_test
