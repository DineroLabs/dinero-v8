#include "mining_liveness_oracle_ml5.h"
#include <sstream>

// Ring 4 Phase 4e: ML5 - Stale Templates Eventually Discarded Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

ML5Oracle::ML5Oracle(const ConsensusParams& params)
    : MiningLivenessOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string ML5Oracle::name() const {
    return "ML5: Stale Templates Eventually Discarded";
}

void ML5Oracle::reset() {
    this->MiningLivenessOracle::reset();

    tip_changed_ = false;
    template_active_ = false;
    template_discarded_ = false;
    events_since_tip_change_ = 0;
    tip_change_event_index_ = 0;
    last_tip_ = 0;
    total_tip_changes_ = 0;
    total_template_discards_ = 0;
}

// ============================================================================
// Event Observation
// ============================================================================

void ML5Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track chain tip changes
    if (state.current_tip != 0 && state.current_tip != last_tip_) {
        // Tip changed
        uint64_t old_tip = last_tip_;
        last_tip_ = state.current_tip;

        // Only flag if we had a template active when tip changed
        if (template_active_ && old_tip != 0) {
            tip_changed_ = true;
            template_discarded_ = false;
            events_since_tip_change_ = 0;
            tip_change_event_index_ = event_index;
            total_tip_changes_++;
        }
    }

    // Track template creation
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        template_active_ = true;
    }

    // Count events while waiting for stale template to be discarded
    if (tip_changed_ && template_active_ && !template_discarded_) {
        events_since_tip_change_++;

        // Check if threshold exceeded without discard
        if (events_since_tip_change_ > kDiscardThreshold) {
            std::ostringstream msg;
            msg << "Chain tip changed at event " << tip_change_event_index_
                << " but stale template not discarded after " << events_since_tip_change_
                << " events (threshold: " << kDiscardThreshold << ")";

            this->reportViolation("ML5", msg.str(), event_index);

            // Reset to avoid repeated violations
            tip_changed_ = false;
        }
    }

    // Track TEMPLATE_DISCARDED events
    if (event.type == MiningEventType::TEMPLATE_DISCARDED) {
        total_template_discards_++;

        if (tip_changed_) {
            // Stale template discarded - forward progress made
            template_discarded_ = true;
            tip_changed_ = false;
        }

        template_active_ = false;
    }

    // Track POW_STOPPED - mining stopped
    // If mining stops, we no longer need to track template discard
    if (event.type == MiningEventType::POW_STOPPED) {
        if (tip_changed_) {
            // Mining stopped, template no longer relevant
            // Don't flag violation
            tip_changed_ = false;
        }
        template_active_ = false;
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void ML5Oracle::finalize() {
    // Check if trace ended with pending stale template discard
    if (tip_changed_ && template_active_ && !template_discarded_) {
        if (events_since_tip_change_ > kDiscardThreshold) {
            std::ostringstream msg;
            msg << "Trace ended with stale template (tip changed at event "
                << tip_change_event_index_ << ") not discarded after "
                << events_since_tip_change_ << " events";

            this->reportViolation("ML5", msg.str(), 0);
        }
    }
}

}  // namespace mining_test
