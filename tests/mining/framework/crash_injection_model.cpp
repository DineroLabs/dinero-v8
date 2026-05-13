#include "crash_injection_model.h"

// Ring 4 Phase 4b: Crash/Restart Injection Implementation
// Rule: NO correctness assertions - just crash/restart semantics

namespace mining_test {

// ============================================================================
// CrashPolicy Presets
// ============================================================================

CrashPolicy CrashPolicy::noCrashes() {
    CrashPolicy policy;
    policy.crash_probability = 0.0;
    policy.max_crashes = 0;
    return policy;
}

CrashPolicy CrashPolicy::singleCrash(double probability) {
    CrashPolicy policy;
    policy.crash_probability = probability;
    policy.max_crashes = 1;
    policy.min_time_between_crashes = 10;
    return policy;
}

CrashPolicy CrashPolicy::frequentCrashes(double probability, uint32_t max_count) {
    CrashPolicy policy;
    policy.crash_probability = probability;
    policy.max_crashes = max_count;
    policy.min_time_between_crashes = 5;
    return policy;
}

CrashPolicy CrashPolicy::crashDuringMining(double probability) {
    CrashPolicy policy;
    policy.crash_probability = probability;
    policy.max_crashes = 3;
    policy.allow_crash_during_assembly = true;
    policy.allow_crash_during_hashing = true;
    policy.allow_crash_during_submission = true;
    policy.allow_crash_during_reorg = false;  // Don't crash during reorg
    return policy;
}

// ============================================================================
// StateClassification
// ============================================================================

StateClassification::VolatileState StateClassification::extractVolatile(const MiningState& state) {
    VolatileState volatile_state;
    volatile_state.template_prev_hash = state.template_prev_hash;
    volatile_state.template_height = state.template_height;
    volatile_state.template_subsidy = state.template_subsidy;
    volatile_state.template_tx_count = state.template_tx_count;
    // Note: hashing_iterations not in MiningState, tracked separately
    return volatile_state;
}

StateClassification::DurableState StateClassification::extractDurable(const MiningState& state) {
    DurableState durable_state;
    durable_state.current_tip = state.current_tip;
    durable_state.current_height = state.current_height;
    durable_state.mempool_size = state.mempool_size;
    durable_state.mempool_total_fees = state.mempool_total_fees;
    durable_state.blocks_found = state.blocks_found;
    durable_state.restart_count = state.restart_count;
    return durable_state;
}

bool StateClassification::verifyDurableStatePreserved(const MiningState& before_crash,
                                                       const MiningState& after_crash) {
    DurableState before = extractDurable(before_crash);
    DurableState after = extractDurable(after_crash);

    return before.current_tip == after.current_tip &&
           before.current_height == after.current_height &&
           before.mempool_size == after.mempool_size &&
           before.mempool_total_fees == after.mempool_total_fees &&
           before.blocks_found == after.blocks_found;
    // Note: restart_count should increment, so don't compare
}

bool StateClassification::verifyVolatileStateCleared(const MiningState& after_crash) {
    // After crash, volatile state should be cleared
    return !after_crash.template_prev_hash.has_value() &&
           !after_crash.template_height.has_value() &&
           !after_crash.template_subsidy.has_value() &&
           !after_crash.template_tx_count.has_value() &&
           after_crash.phase == MiningPhase::STOPPED;
}

// ============================================================================
// CrashInjector
// ============================================================================

CrashInjector::CrashInjector(const CrashPolicy& policy, uint64_t rng_seed)
    : policy_(policy), env_(rng_seed) {
    crashes_injected_ = 0;
    last_crash_time_ = 0;
    currently_crashed_ = false;
}

std::vector<MiningAction> CrashInjector::injectCrashes(const std::vector<MiningAction>& actions) {
    reset();
    std::vector<MiningAction> result;

    for (const auto& action : actions) {
        // If currently crashed and this is a restart, allow it
        if (action.type == MiningActionType::RESTART && currently_crashed_) {
            result.push_back(action);
            currently_crashed_ = false;
            continue;
        }

        // If currently crashed, skip actions (system is down)
        if (currently_crashed_) {
            // Only time can advance while crashed
            if (action.type == MiningActionType::TIME_ADVANCED) {
                result.push_back(action);
            }
            continue;
        }

        // Check if we should inject a crash before this action
        if (shouldCrashNow(action)) {
            result.push_back(createCrashAction(action.timestamp));
            currently_crashed_ = true;
            crashes_injected_++;
            last_crash_time_ = action.timestamp;

            // Inject restart after a delay
            uint64_t restart_delay = 5 + env_.rng().nextInRange(10);
            result.push_back(createRestartAction(action.timestamp + restart_delay));
            currently_crashed_ = false;
        }

        // Add original action
        result.push_back(action);
    }

    return result;
}

bool CrashInjector::shouldCrashNow(const MiningAction& action) {
    // Check if crashes are allowed
    if (policy_.crash_probability <= 0.0) {
        return false;
    }

    // Check if max crashes reached
    if (crashes_injected_ >= policy_.max_crashes) {
        return false;
    }

    // Check minimum time between crashes
    if (action.timestamp - last_crash_time_ < policy_.min_time_between_crashes) {
        return false;
    }

    // Check if crash is allowed at this action
    if (!isCrashAllowed(action)) {
        return false;
    }

    // Probabilistic decision
    double r = env_.rng().nextDouble();
    return r < policy_.crash_probability;
}

bool CrashInjector::isCrashAllowed(const MiningAction& action) const {
    // Crashes cannot occur during certain critical actions
    switch (action.type) {
        case MiningActionType::START_MINING:
            return policy_.allow_crash_during_assembly;

        case MiningActionType::TIME_ADVANCED:
            // Assume TIME_ADVANCED represents hashing
            return policy_.allow_crash_during_hashing;

        case MiningActionType::NEW_BLOCK_ARRIVED:
            return policy_.allow_crash_during_submission;

        case MiningActionType::REORG:
            return policy_.allow_crash_during_reorg;

        case MiningActionType::CRASH:
        case MiningActionType::RESTART:
            // Never inject crash during explicit crash/restart
            return false;

        default:
            return true;
    }
}

MiningAction CrashInjector::createCrashAction(uint64_t timestamp) {
    MiningAction action;
    action.type = MiningActionType::CRASH;
    action.timestamp = timestamp;
    action.description = "Injected crash";
    return action;
}

MiningAction CrashInjector::createRestartAction(uint64_t timestamp) {
    MiningAction action;
    action.type = MiningActionType::RESTART;
    action.timestamp = timestamp;
    action.description = "Injected restart";
    return action;
}

void CrashInjector::reset() {
    env_.reset();
    crashes_injected_ = 0;
    last_crash_time_ = 0;
    currently_crashed_ = false;
}

// ============================================================================
// RestartValidator
// ============================================================================

bool RestartValidator::validateRestart(const MiningState& pre_crash,
                                        const MiningState& post_restart) {
    // Durable state must be preserved
    bool durable_ok = StateClassification::verifyDurableStatePreserved(pre_crash, post_restart);

    // Volatile state must be cleared
    bool volatile_ok = StateClassification::verifyVolatileStateCleared(post_restart);

    // Restart count should increment
    bool restart_count_ok = (post_restart.restart_count == pre_crash.restart_count + 1);

    // Crashed flag should be cleared
    bool crashed_flag_ok = !post_restart.has_crashed;

    return durable_ok && volatile_ok && restart_count_ok && crashed_flag_ok;
}

bool RestartValidator::validateCrash(const MiningState& pre_crash,
                                      const MiningState& post_crash) {
    // Volatile state must be cleared
    bool volatile_ok = StateClassification::verifyVolatileStateCleared(post_crash);

    // Crashed flag should be set
    bool crashed_flag_ok = post_crash.has_crashed;

    // Phase should be STOPPED
    bool phase_ok = (post_crash.phase == MiningPhase::STOPPED);

    return volatile_ok && crashed_flag_ok && phase_ok;
}

RestartValidator::StateDiff RestartValidator::computeDiff(const MiningState& before,
                                                           const MiningState& after) {
    StateDiff diff;

    // Check volatile state
    diff.volatile_cleared = StateClassification::verifyVolatileStateCleared(after);

    // Check durable state
    diff.durable_preserved = StateClassification::verifyDurableStatePreserved(before, after);

    // Record violations
    if (!diff.volatile_cleared) {
        if (after.template_prev_hash.has_value()) {
            diff.violations.push_back("template_prev_hash not cleared");
        }
        if (after.template_height.has_value()) {
            diff.violations.push_back("template_height not cleared");
        }
        if (after.template_subsidy.has_value()) {
            diff.violations.push_back("template_subsidy not cleared");
        }
        if (after.template_tx_count.has_value()) {
            diff.violations.push_back("template_tx_count not cleared");
        }
    }

    if (!diff.durable_preserved) {
        StateClassification::DurableState before_durable = StateClassification::extractDurable(before);
        StateClassification::DurableState after_durable = StateClassification::extractDurable(after);

        if (before_durable.current_tip != after_durable.current_tip) {
            diff.violations.push_back("current_tip changed");
        }
        if (before_durable.current_height != after_durable.current_height) {
            diff.violations.push_back("current_height changed");
        }
        if (before_durable.mempool_size != after_durable.mempool_size) {
            diff.violations.push_back("mempool_size changed");
        }
        if (before_durable.mempool_total_fees != after_durable.mempool_total_fees) {
            diff.violations.push_back("mempool_total_fees changed");
        }
        if (before_durable.blocks_found != after_durable.blocks_found) {
            diff.violations.push_back("blocks_found changed");
        }
    }

    return diff;
}

}  // namespace mining_test
