#include "mining_safety_oracle_ms2.h"
#include <sstream>

// Ring 4 Phase 4d: MS2 - No Duplicate Subsidy Implementation

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MS2Oracle::MS2Oracle(const ConsensusParams& params)
    : MiningSafetyOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MS2Oracle::name() const {
    return "MS2: No Duplicate Subsidy";
}

void MS2Oracle::reset() {
    this->MiningSafetyOracle::reset();

    solutions_by_height_.clear();
    accepted_by_height_.clear();
    subsidy_claimed_by_height_.clear();
}

// ============================================================================
// Event Observation
// ============================================================================

void MS2Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track solutions found
    if (event.type == MiningEventType::SOLUTION_FOUND) {
        if (event.template_height.has_value() && event.block_hash.has_value()) {
            uint32_t height = *event.template_height;
            uint64_t block_hash = *event.block_hash;

            // Record this solution
            solutions_by_height_[height].emplace_back(block_hash, event_index);

            // Check if we already have a different solution at this height
            if (solutions_by_height_[height].size() > 1) {
                // Multiple solutions at same height - potential duplicate subsidy
                std::ostringstream msg;
                msg << "Multiple solutions found at height " << height
                    << " (" << solutions_by_height_[height].size() << " total) - "
                    << "potential duplicate subsidy claim";

                this->reportViolation("MS2", msg.str(), event_index);
            }
        }
    }

    // Track blocks accepted
    if (event.type == MiningEventType::BLOCK_ACCEPTED) {
        if (event.template_height.has_value() && event.block_hash.has_value()) {
            uint32_t height = *event.template_height;
            uint64_t block_hash = *event.block_hash;

            // Record this acceptance
            accepted_by_height_[height].emplace_back(block_hash, event_index);

            // Check if multiple blocks accepted at same height
            if (accepted_by_height_[height].size() > 1) {
                // Multiple blocks accepted at same height
                // This is a serious violation - consensus should prevent this
                std::ostringstream msg;
                msg << "Multiple blocks accepted at height " << height
                    << " (" << accepted_by_height_[height].size() << " blocks) - "
                    << "duplicate subsidy violation (consensus failure)";

                this->reportViolation("MS2", msg.str(), event_index);
            }
        }
    }

    // Track subsidy claims from template creation
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        if (event.template_height.has_value() && event.subsidy_claimed.has_value()) {
            uint32_t height = *event.template_height;
            uint64_t subsidy = *event.subsidy_claimed;

            // Accumulate subsidy claimed at this height
            subsidy_claimed_by_height_[height] += subsidy;
        }
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void MS2Oracle::finalize() {
    // After all events processed, check for duplicate subsidy patterns

    // Check each height that had subsidy claims
    for (const auto& [height, total_claimed] : subsidy_claimed_by_height_) {
        uint64_t expected_subsidy = this->getSubsidyCalculator().getBlockSubsidy(height);

        // If total claimed at this height significantly exceeds expected,
        // that indicates duplicate subsidy claims
        // Allow some tolerance for template recreation (up to 3x for Phase 4d)
        if (total_claimed > expected_subsidy * 3) {
            std::ostringstream msg;
            msg << "Height " << height << " claimed total subsidy "
                << total_claimed << " but expected only " << expected_subsidy
                << " - possible duplicate subsidy (claimed "
                << (total_claimed / expected_subsidy) << "x expected)";

            this->reportViolation("MS2", msg.str(), 0);
        }
    }

    // Cross-check: heights with multiple solutions should have been flagged
    for (const auto& [height, solutions] : solutions_by_height_) {
        if (solutions.size() > 1) {
            // Already flagged in observe(), but verify consistency
            // Check if they're actually different blocks
            bool all_same = true;
            uint64_t first_hash = solutions[0].first;

            for (size_t i = 1; i < solutions.size(); i++) {
                if (solutions[i].first != first_hash) {
                    all_same = false;
                    break;
                }
            }

            if (!all_same) {
                // Multiple different blocks at same height
                std::ostringstream msg;
                msg << "Height " << height << " has " << solutions.size()
                    << " different solutions - indicates chain fork or duplicate work";

                this->reportViolation("MS2", msg.str(), 0);
            }
        }
    }
}

}  // namespace mining_test
