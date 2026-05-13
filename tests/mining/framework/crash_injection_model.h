#pragma once

#include "mining_types.h"
#include "deterministic_env.h"
#include <vector>
#include <functional>

// Ring 4 Phase 4b: Crash/Restart Injection Model
// Purpose: Define crash semantics and injection policies
// Rule: NO correctness assertions, just crash/restart modeling

namespace mining_test {

// ============================================================================
// StateDurability - Defines what state survives crashes
// ============================================================================

enum class StateDurability {
    VOLATILE,    // Lost on crash (memory-only: mining template, hash iterations)
    DURABLE      // Survives crash (disk-persisted: chain tip, mempool)
};

// ============================================================================
// CrashPoint - Defines when crashes can occur
// ============================================================================

enum class CrashPoint {
    DURING_TEMPLATE_ASSEMBLY,   // While building block template
    DURING_HASHING,             // While computing PoW
    DURING_BLOCK_SUBMISSION,    // While submitting found block
    DURING_REORG_HANDLING,      // While processing chain reorg
    ANY_TIME                    // Can crash at any moment
};

// ============================================================================
// CrashPolicy - Defines crash injection rules
// ============================================================================

struct CrashPolicy {
    // Probability of crash at each crash point
    double crash_probability{0.0};

    // Which phases allow crashes
    bool allow_crash_during_assembly{true};
    bool allow_crash_during_hashing{true};
    bool allow_crash_during_submission{true};
    bool allow_crash_during_reorg{true};

    // Minimum time between crashes (to avoid crash loops)
    uint64_t min_time_between_crashes{10};

    // Maximum number of crashes in scenario
    uint32_t max_crashes{0};

    CrashPolicy() = default;

    // Preset policies
    static CrashPolicy noCrashes();
    static CrashPolicy singleCrash(double probability = 0.05);
    static CrashPolicy frequentCrashes(double probability = 0.1, uint32_t max_count = 5);
    static CrashPolicy crashDuringMining(double probability = 0.1);
};

// ============================================================================
// StateClassification - Classifies MiningState fields by durability
// ============================================================================

struct StateClassification {
    // Volatile fields (lost on crash)
    struct VolatileState {
        bool is_mining{false};
        std::optional<uint64_t> template_prev_hash;
        std::optional<uint32_t> template_height;
        std::optional<uint64_t> template_subsidy;
        std::optional<uint32_t> template_tx_count;
        uint64_t hashing_iterations{0};
    };

    // Durable fields (survive crash)
    struct DurableState {
        uint64_t current_tip{0};
        uint32_t current_height{0};
        uint32_t mempool_size{0};
        uint64_t mempool_total_fees{0};
        uint64_t blocks_found{0};
        uint32_t restart_count{0};
    };

    // Extract volatile state from MiningState
    static VolatileState extractVolatile(const MiningState& state);

    // Extract durable state from MiningState
    static DurableState extractDurable(const MiningState& state);

    // Check if state after crash preserves durable fields
    static bool verifyDurableStatePreserved(const MiningState& before_crash,
                                             const MiningState& after_crash);

    // Check if state after crash cleared volatile fields
    static bool verifyVolatileStateCleared(const MiningState& after_crash);
};

// ============================================================================
// CrashInjector - Injects crashes into action sequences
// ============================================================================

class CrashInjector {
public:
    // Construct with policy and seed
    CrashInjector(const CrashPolicy& policy, uint64_t rng_seed);

    // Inject crashes into existing action sequence
    std::vector<MiningAction> injectCrashes(const std::vector<MiningAction>& actions);

    // Decide if crash should occur at this point
    bool shouldCrashNow(const MiningAction& action);

    // Get current policy
    const CrashPolicy& getPolicy() const { return policy_; }

    // Reset state
    void reset();

private:
    // Check if crash is allowed at this action
    bool isCrashAllowed(const MiningAction& action) const;

    // Create crash action at timestamp
    MiningAction createCrashAction(uint64_t timestamp);

    // Create restart action at timestamp
    MiningAction createRestartAction(uint64_t timestamp);

    CrashPolicy policy_;
    DeterministicEnvironment env_;

    // Injection state
    uint32_t crashes_injected_{0};
    uint64_t last_crash_time_{0};
    bool currently_crashed_{false};
};

// ============================================================================
// RestartValidator - Validates restart behavior
// ============================================================================

class RestartValidator {
public:
    // Check if restart correctly preserves durable state
    static bool validateRestart(const MiningState& pre_crash,
                                 const MiningState& post_restart);

    // Check if crash correctly clears volatile state
    static bool validateCrash(const MiningState& pre_crash,
                               const MiningState& post_crash);

    // Extract state differences after crash/restart
    struct StateDiff {
        bool volatile_cleared{false};
        bool durable_preserved{false};
        std::vector<std::string> violations;
    };

    static StateDiff computeDiff(const MiningState& before, const MiningState& after);
};

}  // namespace mining_test
