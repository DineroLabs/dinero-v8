#include "mining_simulator.h"
#include <algorithm>

// Ring 4 Phase 4b: Mock Mining Simulator Implementation
// Rule: NO consensus logic - this is a dumb recorder

namespace mining_test {

// ============================================================================
// Constructor
// ============================================================================

MiningSimulator::MiningSimulator(uint64_t rng_seed)
    : env_(rng_seed), seed_(rng_seed) {
    // Initialize state
    current_state_ = MiningState{};
    current_state_.phase = MiningPhase::STOPPED;
    current_state_.timestamp = env_.now();
}

// ============================================================================
// Action Application
// ============================================================================

void MiningSimulator::applyAction(const MiningAction& action) {
    // Record action
    MiningAction recorded_action = action;
    recorded_action.sequence_number = action_sequence_++;
    recorded_action.timestamp = env_.now();
    actions_.push_back(recorded_action);

    // Update state timestamp
    current_state_.timestamp = env_.now();

    // Dispatch to handler
    switch (action.type) {
        case MiningActionType::START_MINING:
            handleStartMining(action);
            break;
        case MiningActionType::STOP_MINING:
            handleStopMining(action);
            break;
        case MiningActionType::NEW_BLOCK_ARRIVED:
            handleNewBlock(action);
            break;
        case MiningActionType::TX_ADDED_TO_MEMPOOL:
            handleTxAdded(action);
            break;
        case MiningActionType::TX_REMOVED_FROM_MEMPOOL:
            handleTxRemoved(action);
            break;
        case MiningActionType::TIME_ADVANCED:
            handleTimeAdvanced(action);
            break;
        case MiningActionType::CRASH:
            handleCrash(action);
            break;
        case MiningActionType::RESTART:
            handleRestart(action);
            break;
        case MiningActionType::REORG:
            handleReorg(action);
            break;
    }
}

// ============================================================================
// Event Recording
// ============================================================================

void MiningSimulator::recordEvent(MiningEventType type, const std::string& description) {
    MiningEvent event;
    event.type = type;
    event.timestamp = env_.now();
    event.sequence_number = event_sequence_++;
    event.description = description;
    events_.push_back(event);
}

void MiningSimulator::recordEvent(const MiningEvent& event) {
    MiningEvent recorded = event;
    recorded.sequence_number = event_sequence_++;
    recorded.timestamp = env_.now();
    events_.push_back(recorded);
}

// ============================================================================
// Action Handlers
// ============================================================================

void MiningSimulator::handleStartMining(const MiningAction& action) {
    if (current_state_.has_crashed) {
        // Cannot start mining while crashed
        recordEvent(MiningEventType::ERROR_OCCURRED, "Cannot start: system crashed");
        return;
    }

    if (is_mining_) {
        // Already mining, ignore
        return;
    }

    // Create initial template
    createTemplate();

    // Start mining
    is_mining_ = true;
    current_state_.phase = MiningPhase::MINING;
    recordEvent(MiningEventType::POW_STARTED, "Mining started");
}

void MiningSimulator::handleStopMining(const MiningAction& action) {
    if (!is_mining_) {
        // Not mining, ignore
        return;
    }

    // Stop mining
    is_mining_ = false;
    current_state_.phase = MiningPhase::STOPPED;
    hashing_iterations_ = 0;
    recordEvent(MiningEventType::POW_STOPPED, "Mining stopped");

    // Discard template
    if (current_state_.template_prev_hash.has_value()) {
        discardTemplate();
    }
}

void MiningSimulator::handleNewBlock(const MiningAction& action) {
    // Update chain tip
    if (action.block_hash) {
        current_state_.current_tip = *action.block_hash;
    }
    if (action.new_height) {
        current_state_.current_height = *action.new_height;
    }

    // If mining, discard old template and create new one
    if (is_mining_) {
        discardTemplate();
        createTemplate();
    }
}

void MiningSimulator::handleTxAdded(const MiningAction& action) {
    // Update mempool state
    current_state_.mempool_size++;

    // Add random fee (deterministic based on RNG)
    uint64_t tx_fee = env_.rng().nextInRange(1, 1000);
    current_state_.mempool_total_fees += tx_fee;

    // If mining, might refresh template (simplified: always refresh)
    if (is_mining_ && current_state_.template_prev_hash.has_value()) {
        discardTemplate();
        createTemplate();
    }
}

void MiningSimulator::handleTxRemoved(const MiningAction& action) {
    // Update mempool state
    if (current_state_.mempool_size > 0) {
        current_state_.mempool_size--;

        // Remove random fee (deterministic)
        uint64_t tx_fee = env_.rng().nextInRange(1, std::min(uint64_t(1000), current_state_.mempool_total_fees));
        current_state_.mempool_total_fees = (current_state_.mempool_total_fees > tx_fee)
            ? (current_state_.mempool_total_fees - tx_fee)
            : 0;
    }
}

void MiningSimulator::handleTimeAdvanced(const MiningAction& action) {
    // Advance mock clock
    env_.advanceTime(1);

    // If mining, simulate hashing
    if (is_mining_ && current_state_.template_prev_hash.has_value()) {
        tryFindSolution();
    }
}

void MiningSimulator::handleCrash(const MiningAction& action) {
    // Mark as crashed
    current_state_.has_crashed = true;
    current_state_.phase = MiningPhase::STOPPED;
    is_mining_ = false;

    // Discard volatile state (template)
    current_state_.template_prev_hash.reset();
    current_state_.template_height.reset();
    current_state_.template_subsidy.reset();
    current_state_.template_tx_count.reset();
    hashing_iterations_ = 0;

    recordEvent(MiningEventType::ERROR_OCCURRED, "System crashed");
}

void MiningSimulator::handleRestart(const MiningAction& action) {
    if (!current_state_.has_crashed) {
        recordEvent(MiningEventType::ERROR_OCCURRED, "Cannot restart: not crashed");
        return;
    }

    // Clear crashed flag
    current_state_.has_crashed = false;
    current_state_.restart_count++;

    // Durable state (chain tip, mempool) is preserved
    // Volatile state (template, mining) was already cleared in crash

    recordEvent(MiningEventType::ERROR_OCCURRED, "System restarted");
}

void MiningSimulator::handleReorg(const MiningAction& action) {
    // Update chain tip to new fork
    if (action.block_hash) {
        current_state_.current_tip = *action.block_hash;
    }

    // Reorg depth affects height
    if (action.reorg_depth && action.new_height) {
        current_state_.current_height = *action.new_height;
    }

    // If mining, discard old template (stale) and create new
    if (is_mining_) {
        discardTemplate();
        createTemplate();
    }
}

// ============================================================================
// Mock Mining Logic (Deterministic, NOT real PoW)
// ============================================================================

void MiningSimulator::tryFindSolution() {
    // Increment simulated hash count
    hashing_iterations_++;
    current_state_.hashes_computed++;

    // Deterministically "find solution" based on RNG
    // This is NOT real PoW - just simulating finding a solution
    uint64_t solution_threshold = 1000 + (env_.rng().next() % 5000);

    if (hashing_iterations_ >= solution_threshold) {
        // "Found" solution
        hashing_iterations_ = 0;
        last_solution_time_ = env_.now();

        // Record solution found
        MiningEvent event;
        event.type = MiningEventType::SOLUTION_FOUND;
        event.block_hash = env_.rng().next();  // Random placeholder hash
        event.template_height = current_state_.template_height;
        event.subsidy_claimed = current_state_.template_subsidy;
        recordEvent(event);

        // Simulate block submission
        recordEvent(MiningEventType::BLOCK_SUBMITTED, "Block submitted");

        // Deterministically accept or reject (mostly accept)
        bool accepted = env_.rng().nextInRange(1, 100) > 5;  // 95% acceptance rate

        if (accepted) {
            recordEvent(MiningEventType::BLOCK_ACCEPTED, "Block accepted");
            current_state_.blocks_found++;

            // Update chain tip to new block
            if (event.block_hash) {
                current_state_.current_tip = *event.block_hash;
            }
            if (current_state_.template_height) {
                current_state_.current_height = *current_state_.template_height;
            }
        } else {
            recordEvent(MiningEventType::BLOCK_REJECTED, "Block rejected");
        }

        // Create new template for next block
        if (is_mining_) {
            discardTemplate();
            createTemplate();
        }
    }
}

void MiningSimulator::createTemplate() {
    current_state_.phase = MiningPhase::ASSEMBLING;

    // Create template with placeholder values
    current_state_.template_prev_hash = current_state_.current_tip;
    current_state_.template_height = current_state_.current_height + 1;

    // Ring 4 Phase 4b placeholder subsidy
    // Matches current Dinero PoW subsidy numerically (100 DIN),
    // but carries NO consensus meaning.
    // NOT consensus-accurate: no halving, no validation, no enforcement.
    // Used ONLY for deterministic trace recording.
    constexpr uint64_t kPlaceholderSubsidyDIN = 100ULL;
    constexpr uint64_t kDINUnit = 100000000ULL;
    current_state_.template_subsidy = kPlaceholderSubsidyDIN * kDINUnit;

    // Include some transactions from mempool (deterministic selection)
    uint32_t tx_count = std::min(current_state_.mempool_size,
                                  static_cast<uint32_t>(env_.rng().nextInRange(1, 100)));
    current_state_.template_tx_count = tx_count;

    current_state_.templates_created++;
    current_state_.phase = MiningPhase::MINING;

    // Record event
    MiningEvent event;
    event.type = MiningEventType::TEMPLATE_CREATED;
    event.template_height = current_state_.template_height;
    event.subsidy_claimed = current_state_.template_subsidy;
    recordEvent(event);
}

void MiningSimulator::discardTemplate() {
    if (!current_state_.template_prev_hash.has_value()) {
        return;  // No template to discard
    }

    recordEvent(MiningEventType::TEMPLATE_DISCARDED, "Old template discarded");

    // Clear template
    current_state_.template_prev_hash.reset();
    current_state_.template_height.reset();
    current_state_.template_subsidy.reset();
    current_state_.template_tx_count.reset();
    hashing_iterations_ = 0;
}

// ============================================================================
// State Management
// ============================================================================

void MiningSimulator::saveCheckpoint() {
    checkpoints_.push_back(current_state_);
}

void MiningSimulator::restoreLastCheckpoint() {
    if (!checkpoints_.empty()) {
        current_state_ = checkpoints_.back();
        checkpoints_.pop_back();
    }
}

void MiningSimulator::reset() {
    current_state_ = MiningState{};
    current_state_.phase = MiningPhase::STOPPED;
    events_.clear();
    actions_.clear();
    checkpoints_.clear();
    action_sequence_ = 0;
    event_sequence_ = 0;
    is_mining_ = false;
    hashing_iterations_ = 0;
    last_solution_time_ = 0;
    env_.reset();
}

// ============================================================================
// Trace Extraction
// ============================================================================

MiningTrace MiningSimulator::extractTrace() const {
    MiningTrace trace;
    trace.rng_seed = seed_;
    trace.actions = actions_;
    trace.events = events_;
    trace.snapshots = checkpoints_;

    // Add final state as last snapshot
    if (checkpoints_.empty() || !(checkpoints_.back() == current_state_)) {
        trace.snapshots.push_back(current_state_);
    }

    trace.start_time = actions_.empty() ? 0 : actions_.front().timestamp;
    trace.end_time = env_.now();
    trace.completed_successfully = !current_state_.has_crashed;

    // Compute hash
    trace.updateHash();

    return trace;
}

// ============================================================================
// Query
// ============================================================================

std::vector<MiningEvent> MiningSimulator::getEventsSince(uint64_t timestamp) const {
    std::vector<MiningEvent> result;
    for (const auto& event : events_) {
        if (event.timestamp >= timestamp) {
            result.push_back(event);
        }
    }
    return result;
}

}  // namespace mining_test
