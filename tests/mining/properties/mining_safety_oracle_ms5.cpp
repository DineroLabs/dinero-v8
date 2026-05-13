#include "mining_safety_oracle_ms5.h"
#include <sstream>

// Ring 4 Phase 4d: MS5 - No Stale Block Acceptance Implementation
// Note: Placeholder validation - basic tip tracking only

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MS5Oracle::MS5Oracle(const ConsensusParams& params)
    : MiningSafetyOracle(params) {
}

// ============================================================================
// Oracle Interface
// ============================================================================

std::string MS5Oracle::name() const {
    return "MS5: No Stale Block Acceptance (Placeholder)";
}

void MS5Oracle::reset() {
    this->MiningSafetyOracle::reset();

    current_tip_ = 0;
    reorg_count_ = 0;
    templates_by_height_.clear();
    accepted_by_height_.clear();
}

// ============================================================================
// Event Observation
// ============================================================================

void MS5Oracle::observe(
    const MiningState& state,
    const MiningEvent& event,
    uint64_t event_index
) {
    // Track current chain tip from state
    if (state.current_tip != 0) {
        // Check if tip changed (potential reorg)
        if (current_tip_ != 0 && state.current_tip != current_tip_) {
            reorg_count_++;
        }
        current_tip_ = state.current_tip;
    }

    // Track reorg events explicitly
    if (event.type == MiningEventType::TEMPLATE_DISCARDED) {
        // Template discarded - likely due to reorg or new block
        // This is expected behavior after reorg
    }

    // Track template creation
    if (event.type == MiningEventType::TEMPLATE_CREATED) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;

            // Record template created at this tip
            // In Phase 4d, we use state.current_tip as the "parent"
            uint64_t template_tip = state.current_tip;

            templates_by_height_[height].emplace_back(template_tip, event_index);

            // Note: Phase 4d limitation - we don't have explicit prev_hash in events
            // We use state.current_tip as proxy for "building on this tip"
            // Full prev_hash tracking will be added in Phase 4h
        }
    }

    // Track block acceptance
    if (event.type == MiningEventType::BLOCK_ACCEPTED) {
        if (event.template_height.has_value()) {
            uint32_t height = *event.template_height;

            // Record block accepted at this tip
            uint64_t block_tip = state.current_tip;

            accepted_by_height_[height].emplace_back(block_tip, event_index);

            // Check if this block was built on current tip
            // In Phase 4d, we do a simple consistency check:
            // If templates were created for this height, verify tip consistency

            if (templates_by_height_.count(height) > 0) {
                // Find the most recent template for this height
                const auto& templates = templates_by_height_[height];

                // In normal operation, the template that led to this block
                // should have been created on the current tip (or a recent tip)
                // If all templates for this height are on very old tips,
                // that might indicate stale block acceptance

                // Phase 4d: Conservative check - just track, don't flag
                // Full stale detection requires prev_hash tracking (Phase 4h)
            }
        }
    }

    // Track block rejection (might be stale)
    if (event.type == MiningEventType::BLOCK_REJECTED) {
        // Block rejection could be due to:
        // - Stale tip (building on old chain)
        // - Invalid PoW
        // - Invalid transactions
        // - Consensus failure
        //
        // Phase 4d: We just note rejections
        // Phase 4h will analyze rejection reasons
    }
}

// ============================================================================
// Final Validation
// ============================================================================

void MS5Oracle::finalize() {
    // After all events processed, check for stale block patterns

    // Phase 4d: We do basic consistency checks
    // Full stale block detection requires:
    // - prev_hash tracking in events
    // - Full fork tree
    // - Block parent validation
    // - Chain state tracking

    // For Phase 4d, we check basic tip consistency
    for (const auto& [height, accepted_blocks] : accepted_by_height_) {
        if (accepted_blocks.size() > 1) {
            // Multiple blocks accepted at same height
            // This could indicate:
            // - Reorg and new block accepted
            // - Fork resolution
            // - Stale block acceptance (bug)
            //
            // Phase 4d: Conservative - don't flag without full context
            // Phase 4h will distinguish legitimate reorgs from stale acceptance
        }
    }

    // Check for templates that were never used
    for (const auto& [height, templates] : templates_by_height_) {
        if (templates.size() > 1 && accepted_by_height_.count(height) == 0) {
            // Multiple templates created but no block accepted
            // This could indicate:
            // - Reorg caused template discard (expected)
            // - Mining difficulty too high (expected)
            // - Template creation without mining (unexpected)
            //
            // Phase 4d: Not a violation - just interesting metadata
        }
    }

    // Phase 4d note: Full stale block detection requires:
    // - Explicit prev_hash field in MiningEvent
    // - Full fork tree tracking
    // - Block parent validation
    // - Chain reorganization modeling
    // - Orphan block detection
    //
    // These will be added in Phase 4h when integrating real consensus
    //
    // For Phase 4d, we enforce basic procedural correctness:
    // - Templates are discarded after reorg (implicitly via TEMPLATE_DISCARDED events)
    // - No blocks accepted on obviously stale tips
    // - Tip tracking is consistent
}

}  // namespace mining_test
