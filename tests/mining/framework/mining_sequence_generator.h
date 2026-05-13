#pragma once

#include "mining_types.h"
#include "deterministic_env.h"
#include <vector>
#include <memory>

// Ring 4 Phase 4b: Mining Sequence Generator
// Purpose: Generate deterministic action sequences for testing
// Rule: NO correctness assertions, just scenario generation

namespace mining_test {

// ============================================================================
// ScenarioConfig - Configuration for generated scenarios
// ============================================================================

struct ScenarioConfig {
    uint64_t rng_seed{42};
    uint32_t num_actions{100};

    // Action probabilities (0.0 to 1.0)
    double prob_start_mining{0.1};
    double prob_stop_mining{0.05};
    double prob_new_block{0.2};
    double prob_tx_added{0.3};
    double prob_tx_removed{0.1};
    double prob_time_advanced{0.2};
    double prob_crash{0.03};
    double prob_restart{0.01};
    double prob_reorg{0.01};

    // Scenario parameters
    uint32_t max_reorg_depth{10};
    uint32_t starting_height{100};
    uint64_t starting_tip_hash{0xDEADBEEF};

    ScenarioConfig() = default;

    // Normalize probabilities to sum to 1.0
    void normalize();
};

// ============================================================================
// MiningSequenceGenerator - Deterministic scenario generator
// ============================================================================

class MiningSequenceGenerator {
public:
    // Construct with seed
    explicit MiningSequenceGenerator(uint64_t rng_seed);

    // Generate predefined scenarios
    std::vector<MiningAction> generateSimpleScenario();
    std::vector<MiningAction> generateRestartScenario();
    std::vector<MiningAction> generateReorgScenario();
    std::vector<MiningAction> generateCrashScenario();
    std::vector<MiningAction> generateMempoolChurnScenario();

    // Generate random scenario with weighted actions
    std::vector<MiningAction> generateRandomScenario(const ScenarioConfig& config);

    // Generate random scenario with default config
    std::vector<MiningAction> generateRandomScenario(uint32_t num_actions);

    // Get current seed
    uint64_t getSeed() const { return seed_; }

    // Reset RNG to original seed
    void reset();

private:
    // Helper: Create specific action types
    MiningAction createStartMining(uint64_t timestamp);
    MiningAction createStopMining(uint64_t timestamp);
    MiningAction createNewBlock(uint64_t timestamp, uint64_t block_hash, uint32_t height);
    MiningAction createTxAdded(uint64_t timestamp, uint64_t tx_hash);
    MiningAction createTxRemoved(uint64_t timestamp, uint64_t tx_hash);
    MiningAction createTimeAdvanced(uint64_t timestamp);
    MiningAction createCrash(uint64_t timestamp);
    MiningAction createRestart(uint64_t timestamp);
    MiningAction createReorg(uint64_t timestamp, uint64_t new_tip, uint32_t depth, uint32_t new_height);

    // Helper: Select random action based on probabilities
    MiningAction selectRandomAction(uint64_t timestamp, const ScenarioConfig& config);

    // State tracking for scenario generation
    uint64_t current_timestamp_{0};
    uint32_t current_height_{100};
    uint64_t current_tip_{0xDEADBEEF};
    bool is_mining_{false};
    bool has_crashed_{false};
    uint32_t mempool_size_{0};

    // Deterministic environment
    DeterministicEnvironment env_;
    uint64_t seed_;
};

}  // namespace mining_test
