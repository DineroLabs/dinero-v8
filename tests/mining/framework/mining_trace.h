#pragma once

#include "mining_types.h"
#include <functional>
#include <vector>

// Ring 4 Phase 4b: Mining Trace (Execution History)
// Purpose: Complete record of mining scenario execution
// Rule: NO mining correctness logic, NO consensus code

namespace mining_test {

// ============================================================================
// MiningTrace - Complete execution history
// ============================================================================

struct MiningTrace {
    // Configuration
    uint64_t rng_seed{0};
    std::string scenario_name;

    // Execution history
    std::vector<MiningAction> actions;      // Inputs
    std::vector<MiningEvent> events;        // Outputs
    std::vector<MiningState> snapshots;     // State at checkpoints

    // Determinism verification
    uint64_t final_hash{0};  // Hash of entire trace

    // Metadata
    uint64_t start_time{0};
    uint64_t end_time{0};
    bool completed_successfully{false};
    std::optional<std::string> failure_reason;

    // Default constructor
    MiningTrace() = default;

    // Helper constructor
    explicit MiningTrace(uint64_t seed, std::string name = "")
        : rng_seed(seed), scenario_name(std::move(name)) {}

    // Compute hash of trace (for determinism verification)
    uint64_t computeHash() const {
        // Simple hash combining all trace elements
        // This is NOT cryptographic, just for determinism checking
        uint64_t hash = rng_seed;

        // Hash actions
        for (const auto& action : actions) {
            hash ^= static_cast<uint64_t>(action.type);
            hash ^= action.timestamp;
            hash ^= action.sequence_number;
            if (action.block_hash) hash ^= *action.block_hash;
            if (action.tx_hash) hash ^= *action.tx_hash;
            if (action.reorg_depth) hash ^= *action.reorg_depth;
            if (action.new_height) hash ^= *action.new_height;
        }

        // Hash events
        for (const auto& event : events) {
            hash ^= static_cast<uint64_t>(event.type);
            hash ^= event.timestamp;
            hash ^= event.sequence_number;
            if (event.block_hash) hash ^= *event.block_hash;
            if (event.template_height) hash ^= *event.template_height;
            if (event.subsidy_claimed) hash ^= *event.subsidy_claimed;
        }

        // Hash final state (if snapshots exist)
        if (!snapshots.empty()) {
            const auto& final_state = snapshots.back();
            hash ^= static_cast<uint64_t>(final_state.phase);
            hash ^= final_state.current_tip;
            hash ^= final_state.current_height;
            hash ^= final_state.hashes_computed;
            hash ^= final_state.blocks_found;
            hash ^= final_state.restart_count;
        }

        return hash;
    }

    // Update final_hash field
    void updateHash() {
        final_hash = computeHash();
    }

    // Equality for exact trace comparison
    bool operator==(const MiningTrace& other) const {
        return rng_seed == other.rng_seed &&
               actions == other.actions &&
               events == other.events &&
               snapshots == other.snapshots;
    }

    // Check if traces have same hash (fast determinism check)
    bool hasSameHashAs(const MiningTrace& other) const {
        return final_hash == other.final_hash;
    }
};

}  // namespace mining_test
