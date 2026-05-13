#include "mining_safety_oracle_ms4.h"
#include <sstream>

// Ring 4 Phase 4d: MS4 - Consensus Always Enforced Implementation
// Note: Placeholder validation - procedural correctness only

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MS4Oracle::MS4Oracle(const ConsensusParams& params)
    : MiningSafetyOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MS4Oracle::name() const {
    return "MS4: Consensus Always Enforced (Placeholder)";
}

void MS4Oracle::reset() {
    this->MiningSafetyOracle::reset();

    is_crashed_ = false;
    heights_with_templates_.clear();
    heights_with_solutions_.clear();
}

// ============================================================================
// Event Observation
// ============================================================================

void MS4Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track crash state
    if (event.type == MiningEventType::ERROR_OCCURRED) {
        if (event.description.find("crashed") != std::string::npos) {
            is_crashed_ = true;
        }

        if (event.description.find("restarted") != std::string::npos) {
            is_crashed_ = false;
        }
    }

    // Track template creation (validation setup)
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;
            heights_with_templates_.insert(height);

            // Check if template created while crashed (validation bypass!)
            if (is_crashed_) {
                std::ostringstream msg;
                msg << "Template created at height " << height
                    << " while system is crashed - validation bypassed";

                this->reportViolation("MS4", msg.str(), event_index);
            }
        }
    }

    // Track solution found (ready for validation)
    if (event.type == MiningEventType::SOLUTION_FOUND) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;
            heights_with_solutions_.insert(height);

            // Check if solution found while crashed (validation bypass!)
            if (is_crashed_) {
                std::ostringstream msg;
                msg << "Solution found at height " << height
                    << " while system is crashed - validation bypassed";

                this->reportViolation("MS4", msg.str(), event_index);
            }
        }
    }

    // Track block acceptance and enforce procedural correctness
    if (event.type == MiningEventType::BLOCK_ACCEPTED) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;

            // CRITICAL: Block accepted while crashed is a validation bypass
            if (is_crashed_) {
                std::ostringstream msg;
                msg << "Block accepted at height " << height
                    << " while system is crashed - consensus validation bypassed";

                this->reportViolation("MS4", msg.str(), event_index);
            }

            // CRITICAL: Block accepted without template creation is a bypass
            if (heights_with_templates_.count(height) == 0) {
                std::ostringstream msg;
                msg << "Block accepted at height " << height
                    << " without prior template creation - validation step skipped";

                this->reportViolation("MS4", msg.str(), event_index);
            }

            // Note: In Phase 4d, we don't have explicit BLOCK_VALIDATED events
            // We rely on the procedural requirement that:
            // 1. Template must be created (validation setup)
            // 2. System must not be crashed (validation possible)
            // 3. Block acceptance implies validation occurred
            //
            // Phase 4h will add explicit validation events and full consensus checks
        }
    }

    // Track block rejection (validation occurred but failed)
    if (event.type == MiningEventType::BLOCK_REJECTED) {
        // Rejection means validation occurred and found the block invalid
        // This is correct behavior - no violation
        // Phase 4h will track why validation failed
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void MS4Oracle::finalize() {
    // After all events processed, verify no violations occurred

    // Check if any blocks were accepted while crashed
    // (This should have been caught in observe(), but double-check)

    // Phase 4d note: Full consensus enforcement requires:
    // - Explicit BLOCK_VALIDATED events
    // - Header validation checks
    // - Block structure validation
    // - Transaction validity checks
    // - UTXO validation
    // - Script execution
    // - Signature verification
    //
    // These will be added in Phase 4h when integrating real consensus code
    //
    // For Phase 4d, we enforce PROCEDURAL correctness:
    // - Validation steps must occur (template creation)
    // - No bypass due to crash
    // - No fast-path shortcuts
    //
    // This catches real bugs:
    // - "We assumed validation ran"
    // - Restart bypasses validation
    // - Crash recovery accepts stale data
}

}  // namespace mining_test
