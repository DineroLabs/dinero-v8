#include "mining_sequence_generator.h"
#include <algorithm>
#include <cmath>

// Ring 4 Phase 4b: Mining Sequence Generator Implementation
// Rule: NO correctness assertions - just generate action sequences

namespace mining_test {

// ============================================================================
// ScenarioConfig
// ============================================================================

void ScenarioConfig::normalize() {
    double sum = prob_start_mining + prob_stop_mining + prob_new_block +
                 prob_tx_added + prob_tx_removed + prob_time_advanced +
                 prob_crash + prob_restart + prob_reorg;

    if (sum > 0.0) {
        prob_start_mining /= sum;
        prob_stop_mining /= sum;
        prob_new_block /= sum;
        prob_tx_added /= sum;
        prob_tx_removed /= sum;
        prob_time_advanced /= sum;
        prob_crash /= sum;
        prob_restart /= sum;
        prob_reorg /= sum;
    }
}

// ============================================================================
// Constructor
// ============================================================================

MiningSequenceGenerator::MiningSequenceGenerator(uint64_t rng_seed)
    : env_(rng_seed), seed_(rng_seed) {
    current_timestamp_ = 0;
    current_height_ = 100;
    current_tip_ = 0xDEADBEEF;
    is_mining_ = false;
    has_crashed_ = false;
    mempool_size_ = 0;
}

// ============================================================================
// Predefined Scenarios
// ============================================================================

std::vector<MiningAction> MiningSequenceGenerator::generateSimpleScenario() {
    reset();
    std::vector<MiningAction> actions;

    // Simple mining flow: start → mine → find block → stop
    actions.push_back(createStartMining(current_timestamp_++));

    // Simulate some hashing time
    for (int i = 0; i < 10; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // New block arrives (someone else found it)
    actions.push_back(createNewBlock(current_timestamp_++, env_.rng().next(), current_height_ + 1));

    // Continue mining on new tip
    for (int i = 0; i < 10; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // Stop mining
    actions.push_back(createStopMining(current_timestamp_++));

    return actions;
}

std::vector<MiningAction> MiningSequenceGenerator::generateRestartScenario() {
    reset();
    std::vector<MiningAction> actions;

    // Start mining
    actions.push_back(createStartMining(current_timestamp_++));

    // Mine for a while
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // Crash while mining
    actions.push_back(createCrash(current_timestamp_++));

    // Restart
    actions.push_back(createRestart(current_timestamp_++));

    // Resume mining
    actions.push_back(createStartMining(current_timestamp_++));

    // Continue mining
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    actions.push_back(createStopMining(current_timestamp_++));

    return actions;
}

std::vector<MiningAction> MiningSequenceGenerator::generateReorgScenario() {
    reset();
    std::vector<MiningAction> actions;

    // Start mining on main chain
    actions.push_back(createStartMining(current_timestamp_++));

    // Mine for a while
    for (int i = 0; i < 10; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // Reorg: different chain becomes the tip
    uint32_t reorg_depth = 3;
    uint64_t new_tip = env_.rng().next();
    actions.push_back(createReorg(current_timestamp_++, new_tip, reorg_depth, current_height_ - reorg_depth + 1));

    // Continue mining on new chain
    for (int i = 0; i < 10; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    actions.push_back(createStopMining(current_timestamp_++));

    return actions;
}

std::vector<MiningAction> MiningSequenceGenerator::generateCrashScenario() {
    reset();
    std::vector<MiningAction> actions;

    // Start mining
    actions.push_back(createStartMining(current_timestamp_++));

    // Mine for a while
    for (int i = 0; i < 10; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // Crash during mining (template should be lost)
    actions.push_back(createCrash(current_timestamp_++));

    // Time passes while crashed
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // Restart
    actions.push_back(createRestart(current_timestamp_++));

    // Start mining again (new template needed)
    actions.push_back(createStartMining(current_timestamp_++));

    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    return actions;
}

std::vector<MiningAction> MiningSequenceGenerator::generateMempoolChurnScenario() {
    reset();
    std::vector<MiningAction> actions;

    // Start mining
    actions.push_back(createStartMining(current_timestamp_++));

    // Add transactions to mempool
    for (int i = 0; i < 20; i++) {
        actions.push_back(createTxAdded(current_timestamp_++, env_.rng().next()));
    }

    // Mine for a bit
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // Remove some transactions (e.g., conflicts, expiration)
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTxRemoved(current_timestamp_++, env_.rng().next()));
    }

    // Mine more
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    // New block arrives, removing transactions from mempool
    actions.push_back(createNewBlock(current_timestamp_++, env_.rng().next(), current_height_ + 1));

    // Continue mining
    for (int i = 0; i < 5; i++) {
        actions.push_back(createTimeAdvanced(current_timestamp_++));
    }

    actions.push_back(createStopMining(current_timestamp_++));

    return actions;
}

// ============================================================================
// Random Scenario Generation
// ============================================================================

std::vector<MiningAction> MiningSequenceGenerator::generateRandomScenario(uint32_t num_actions) {
    ScenarioConfig config;
    config.rng_seed = seed_;
    config.num_actions = num_actions;
    config.normalize();
    return generateRandomScenario(config);
}

std::vector<MiningAction> MiningSequenceGenerator::generateRandomScenario(const ScenarioConfig& config) {
    reset();
    std::vector<MiningAction> actions;

    // Initialize state from config
    current_height_ = config.starting_height;
    current_tip_ = config.starting_tip_hash;

    // Generate requested number of actions
    for (uint32_t i = 0; i < config.num_actions; i++) {
        MiningAction action = selectRandomAction(current_timestamp_, config);
        actions.push_back(action);
        current_timestamp_++;
    }

    return actions;
}

MiningAction MiningSequenceGenerator::selectRandomAction(uint64_t timestamp, const ScenarioConfig& config) {
    // Select action based on weighted probabilities
    double r = env_.rng().nextDouble();
    double cumulative = 0.0;

    // START_MINING
    cumulative += config.prob_start_mining;
    if (r < cumulative && !is_mining_ && !has_crashed_) {
        return createStartMining(timestamp);
    }

    // STOP_MINING
    cumulative += config.prob_stop_mining;
    if (r < cumulative && is_mining_) {
        return createStopMining(timestamp);
    }

    // NEW_BLOCK_ARRIVED
    cumulative += config.prob_new_block;
    if (r < cumulative && !has_crashed_) {
        return createNewBlock(timestamp, env_.rng().next(), current_height_ + 1);
    }

    // TX_ADDED_TO_MEMPOOL
    cumulative += config.prob_tx_added;
    if (r < cumulative && !has_crashed_) {
        return createTxAdded(timestamp, env_.rng().next());
    }

    // TX_REMOVED_FROM_MEMPOOL
    cumulative += config.prob_tx_removed;
    if (r < cumulative && mempool_size_ > 0 && !has_crashed_) {
        return createTxRemoved(timestamp, env_.rng().next());
    }

    // TIME_ADVANCED
    cumulative += config.prob_time_advanced;
    if (r < cumulative) {
        return createTimeAdvanced(timestamp);
    }

    // CRASH
    cumulative += config.prob_crash;
    if (r < cumulative && !has_crashed_) {
        return createCrash(timestamp);
    }

    // RESTART
    cumulative += config.prob_restart;
    if (r < cumulative && has_crashed_) {
        return createRestart(timestamp);
    }

    // REORG
    cumulative += config.prob_reorg;
    if (r < cumulative && !has_crashed_) {
        uint32_t depth = 1 + (env_.rng().nextInRange(config.max_reorg_depth - 1));
        uint64_t new_tip = env_.rng().next();
        uint32_t new_height = (current_height_ >= depth) ? (current_height_ - depth + 1) : 1;
        return createReorg(timestamp, new_tip, depth, new_height);
    }

    // Default: TIME_ADVANCED (safe fallback)
    return createTimeAdvanced(timestamp);
}

// ============================================================================
// Action Creators
// ============================================================================

MiningAction MiningSequenceGenerator::createStartMining(uint64_t timestamp) {
    MiningAction action(MiningActionType::START_MINING, timestamp);
    action.description = "Start mining";
    is_mining_ = true;
    return action;
}

MiningAction MiningSequenceGenerator::createStopMining(uint64_t timestamp) {
    MiningAction action(MiningActionType::STOP_MINING, timestamp);
    action.description = "Stop mining";
    is_mining_ = false;
    return action;
}

MiningAction MiningSequenceGenerator::createNewBlock(uint64_t timestamp, uint64_t block_hash, uint32_t height) {
    MiningAction action(MiningActionType::NEW_BLOCK_ARRIVED, timestamp);
    action.block_hash = block_hash;
    action.new_height = height;
    action.description = "New block arrived";

    // Update internal state
    current_tip_ = block_hash;
    current_height_ = height;

    return action;
}

MiningAction MiningSequenceGenerator::createTxAdded(uint64_t timestamp, uint64_t tx_hash) {
    MiningAction action(MiningActionType::TX_ADDED_TO_MEMPOOL, timestamp);
    action.tx_hash = tx_hash;
    action.description = "Transaction added to mempool";
    mempool_size_++;
    return action;
}

MiningAction MiningSequenceGenerator::createTxRemoved(uint64_t timestamp, uint64_t tx_hash) {
    MiningAction action(MiningActionType::TX_REMOVED_FROM_MEMPOOL, timestamp);
    action.tx_hash = tx_hash;
    action.description = "Transaction removed from mempool";
    if (mempool_size_ > 0) {
        mempool_size_--;
    }
    return action;
}

MiningAction MiningSequenceGenerator::createTimeAdvanced(uint64_t timestamp) {
    MiningAction action(MiningActionType::TIME_ADVANCED, timestamp);
    action.description = "Time advanced";
    return action;
}

MiningAction MiningSequenceGenerator::createCrash(uint64_t timestamp) {
    MiningAction action(MiningActionType::CRASH, timestamp);
    action.description = "System crashed";
    has_crashed_ = true;
    is_mining_ = false;
    return action;
}

MiningAction MiningSequenceGenerator::createRestart(uint64_t timestamp) {
    MiningAction action(MiningActionType::RESTART, timestamp);
    action.description = "System restarted";
    has_crashed_ = false;
    return action;
}

MiningAction MiningSequenceGenerator::createReorg(uint64_t timestamp, uint64_t new_tip, uint32_t depth, uint32_t new_height) {
    MiningAction action(MiningActionType::REORG, timestamp);
    action.block_hash = new_tip;
    action.reorg_depth = depth;
    action.new_height = new_height;
    action.description = "Chain reorganization (depth=" + std::to_string(depth) + ")";

    // Update internal state
    current_tip_ = new_tip;
    current_height_ = new_height;

    return action;
}

// ============================================================================
// Reset
// ============================================================================

void MiningSequenceGenerator::reset() {
    env_.reset();
    current_timestamp_ = 0;
    current_height_ = 100;
    current_tip_ = 0xDEADBEEF;
    is_mining_ = false;
    has_crashed_ = false;
    mempool_size_ = 0;
}

}  // namespace mining_test
