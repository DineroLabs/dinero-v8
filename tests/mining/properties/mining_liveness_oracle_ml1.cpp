#include "mining_liveness_oracle_ml1.h"
#include <sstream>

// Ring 4 Phase 4e: ML1 - Templates Eventually Created Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

ML1Oracle::ML1Oracle(const ConsensusParams& params)
    : MiningLivenessOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string ML1Oracle::name() const {
    return "ML1: Templates Eventually Created";
}

void ML1Oracle::reset() {
    this->MiningLivenessOracle::reset();

    mining_started_ = false;
    events_since_mining_start_ = 0;
    mining_start_event_index_ = 0;
    template_created_ = false;
}

// ============================================================================
// Event Observation
// ============================================================================

void ML1Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track START_MINING actions
    if (event.type == MiningEventType::POW_STARTED) {
        mining_started_ = true;
        events_since_mining_start_ = 0;
        mining_start_event_index_ = event_index;
        template_created_ = false;
    }

    // Count events while mining
    if (mining_started_) {
        events_since_mining_start_++;

        // Check if threshold exceeded without template creation
        if (!template_created_ && events_since_mining_start_ > kTemplateCreationThreshold) {
            std::ostringstream msg;
            msg << "Mining started at event " << mining_start_event_index_
                << " but no template created after " << events_since_mining_start_
                << " events (threshold: " << kTemplateCreationThreshold << ")";

            this->reportViolation("ML1", msg.str(), event_index);

            // Reset to avoid repeated violations
            mining_started_ = false;
        }
    }

    // Track TEMPLATE_CREATED events
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        template_created_ = true;
        mining_started_ = false;  // Template created, progress made
    }

    // Track STOP_MINING
    if (event.type == MiningEventType::POW_STOPPED) {
        // Mining stopped
        // If we didn't create a template, that might be OK
        // (user stopped mining before template was needed)
        // Only flag if mining ran for extended period
        if (mining_started_ && !template_created_) {
            if (events_since_mining_start_ > kTemplateCreationThreshold) {
                std::ostringstream msg;
                msg << "Mining ran for " << events_since_mining_start_
                    << " events without creating template before stopping";

                this->reportViolation("ML1", msg.str(), event_index);
            }
        }

        mining_started_ = false;
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void ML1Oracle::finalize() {
    // Check if trace ended while mining without template
    if (mining_started_ && !template_created_) {
        if (events_since_mining_start_ > kTemplateCreationThreshold) {
            std::ostringstream msg;
            msg << "Trace ended while mining (started at event "
                << mining_start_event_index_ << ") without creating template after "
                << events_since_mining_start_ << " events";

            this->reportViolation("ML1", msg.str(), 0);
        }
    }
}

}  // namespace mining_test
