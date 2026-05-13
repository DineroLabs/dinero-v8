#include "mining_safety_oracle_ms1.h"
#include <sstream>

// Ring 4 Phase 4d: MS1 - No Inflation Under Restart Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MS1Oracle::MS1Oracle(const ConsensusParams& params)
    : MiningSafetyOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MS1Oracle::name() const {
    return "MS1: No Inflation Under Restart";
}

void MS1Oracle::reset() {
    this->MiningSafetyOracle::reset();

    total_subsidy_claimed_ = 0;
    total_subsidy_expected_ = 0;
    restart_count_ = 0;
    currently_crashed_ = false;
    subsidy_by_height_.clear();
}

// ============================================================================
// Event Observation
// ============================================================================

void MS1Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track crash events
    if (event.type == MiningEventType::ERROR_OCCURRED) {
        if (event.description.find("crashed") != std::string::npos) {
            currently_crashed_ = true;
        }

        if (event.description.find("restarted") != std::string::npos) {
            currently_crashed_ = false;
            restart_count_++;
        }
    }

    // Track subsidy claims from template creation
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        if (event.subsidy_claimed.has_value() && event.template_height.has_value()) {
            uint64_t claimed = *event.subsidy_claimed;
            uint32_t height = *event.template_height;

            // Accumulate total claimed
            total_subsidy_claimed_ += claimed;

            // Track by height
            if (subsidy_by_height_.count(height) > 0) {
                // Multiple templates at same height
                // This is OK (templates can be recreated), but track it
                subsidy_by_height_[height] += claimed;
            } else {
                subsidy_by_height_[height] = claimed;
            }

            // Check if claimed matches consensus
            uint64_t expected = this->getSubsidyCalculator().getBlockSubsidy(height);

            if (claimed != expected) {
                std::ostringstream msg;
                msg << "Template at height " << height
                    << " claims subsidy " << claimed
                    << " but consensus requires " << expected
                    << " (after " << restart_count_ << " restarts)";

                this->reportViolation("MS1", msg.str(), event_index);
            }
        }
    }

    // Track expected subsidy from solutions found
    if (event.type == MiningEventType::SOLUTION_FOUND) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;
            uint64_t expected = this->getSubsidyCalculator().getBlockSubsidy(height);

            // Accumulate total expected
            total_subsidy_expected_ += expected;
        }
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void MS1Oracle::finalize() {
    // After all events processed, check for inflation

    // Note: total_subsidy_claimed can be > total_subsidy_expected because
    // multiple templates can be created but only some find solutions.
    // Real inflation is when same solution's subsidy claimed multiple times.

    // For Phase 4d, we do a conservative check:
    // If we found solutions, claimed should match expected
    if (total_subsidy_expected_ > 0) {
        // Check if claimed significantly exceeds expected
        // Allow some tolerance for template recreation
        // (multiple templates can be created for same height)

        // Conservative threshold: claimed should not be more than 10x expected
        // (in normal operation, claimed == expected or slightly higher)
        uint64_t max_reasonable_claimed = total_subsidy_expected_ * 10;

        if (total_subsidy_claimed_ > max_reasonable_claimed) {
            std::ostringstream msg;
            msg << "Total subsidy claimed (" << total_subsidy_claimed_
                << ") significantly exceeds expected (" << total_subsidy_expected_
                << ") after " << restart_count_ << " restarts - possible inflation";

            this->reportViolation("MS1", msg.str(), 0);
        }
    }

    // Check for duplicate subsidy at same height
    for (const auto& [height, total_claimed] : subsidy_by_height_) {
        uint64_t expected = this->getSubsidyCalculator().getBlockSubsidy(height);

        // If total claimed at this height exceeds expected significantly,
        // that indicates multiple templates claiming subsidy incorrectly
        if (total_claimed > expected * 5) {  // Conservative threshold
            std::ostringstream msg;
            msg << "Height " << height << " claimed total subsidy "
                << total_claimed << " but expected only " << expected
                << " - possible duplicate subsidy from restart";

            this->reportViolation("MS1", msg.str(), 0);
        }
    }
}

}  // namespace mining_test
