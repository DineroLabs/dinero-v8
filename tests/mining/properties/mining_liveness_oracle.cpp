#include "mining_liveness_oracle.h"

// Ring 4 Phase 4e: Mining Liveness Oracle Base Class Implementation
// Rule: Liveness means "something good eventually happens"

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MiningLivenessOracle::MiningLivenessOracle(const ConsensusParams& params)
    : params_(params) {
}

// ============================================================================
// Base Class Lifecycle Methods
// ============================================================================

void MiningLivenessOracle::reset() {
    // Clear violations from previous run
    violations_.clear();
}

void MiningLivenessOracle::finalize() {
    // Default: no end-of-trace checks
    // Derived classes can override for final validation
}

// ============================================================================
// Main Check Method - Orchestrates Trace Evaluation
// ============================================================================

std::vector<LivenessViolation> MiningLivenessOracle::check(const MiningTrace& trace) {
    // Reset state before processing new trace
    reset();

    // Build initial state from first snapshot
    MiningState current_state;
    if (!trace.snapshots.empty()) {
        current_state = trace.snapshots.front();
    }

    // Process each event in sequence
    uint64_t event_index = 0;
    size_t snapshot_index = 0;

    for (const auto& event : trace.events) {
        // Update current state from snapshots
        // Snapshots are ordered and correspond to state after each event
        if (snapshot_index < trace.snapshots.size()) {
            current_state = trace.snapshots[snapshot_index];
            snapshot_index++;
        }

        // Let derived class observe this event
        observe(current_state, event, event_index);

        event_index++;
    }

    // Allow derived class to perform final checks
    finalize();

    // Return accumulated violations
    return violations_;
}

// ============================================================================
// Protected Helper Methods
// ============================================================================

void MiningLivenessOracle::reportViolation(
    const std::string& property,
    const std::string& message,
    uint64_t event_index
) {
    violations_.emplace_back(property, message, event_index);
}

}  // namespace mining_test
