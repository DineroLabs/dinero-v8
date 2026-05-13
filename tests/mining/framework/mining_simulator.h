#pragma once

#include "mining_types.h"
#include "mining_trace.h"
#include "deterministic_env.h"
#include <memory>

// Ring 4 Phase 4b: Mock Mining Simulator
// Purpose: Dumb simulator that records events without validating
// Rule: NO consensus logic, NO block validation, NO subsidy checking

namespace mining_test {

// ============================================================================
// MiningSimulator - Deterministic mining environment simulator
// ============================================================================

class MiningSimulator {
public:
    // Construct with seed
    explicit MiningSimulator(uint64_t rng_seed);

    // Apply action to simulator
    void applyAction(const MiningAction& action);

    // Get current state snapshot
    MiningState getCurrentState() const { return current_state_; }

    // Get events since given timestamp
    std::vector<MiningEvent> getEventsSince(uint64_t timestamp) const;

    // Get all events
    const std::vector<MiningEvent>& getAllEvents() const { return events_; }

    // State management
    void saveCheckpoint();
    void restoreLastCheckpoint();
    void reset();

    // Trace extraction
    MiningTrace extractTrace() const;

    // Get current time
    uint64_t now() const { return env_.now(); }

private:
    // Event recording
    void recordEvent(MiningEventType type, const std::string& description = "");
    void recordEvent(const MiningEvent& event);

    // Action handlers
    void handleStartMining(const MiningAction& action);
    void handleStopMining(const MiningAction& action);
    void handleNewBlock(const MiningAction& action);
    void handleTxAdded(const MiningAction& action);
    void handleTxRemoved(const MiningAction& action);
    void handleTimeAdvanced(const MiningAction& action);
    void handleCrash(const MiningAction& action);
    void handleRestart(const MiningAction& action);
    void handleReorg(const MiningAction& action);

    // Mock mining logic (deterministic, not real PoW)
    void tryFindSolution();
    void createTemplate();
    void discardTemplate();

    // Deterministic environment
    DeterministicEnvironment env_;
    uint64_t seed_;

    // State
    MiningState current_state_;

    // Event log
    std::vector<MiningEvent> events_;

    // Action log
    std::vector<MiningAction> actions_;

    // Checkpoints
    std::vector<MiningState> checkpoints_;

    // Sequence counters
    uint64_t action_sequence_{0};
    uint64_t event_sequence_{0};

    // Mock mining state
    bool is_mining_{false};
    uint64_t hashing_iterations_{0};  // Simulated hash count
    uint64_t last_solution_time_{0};  // For deterministic solution finding
};

}  // namespace mining_test
