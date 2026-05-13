/**
 * @file test_mining_restart_policy_tripwires.cpp
 * @brief Mining Restart Policy Tripwire Tests (Phase E.3)
 *
 * Purpose: Ensure restart semantics cannot be weakened
 *
 * CRITICAL: These tests MUST NEVER be modified to "make them pass"
 * If a test fails, it means a policy contract was violated.
 * FIX THE CODE, NOT THE TEST.
 *
 * Tripwires for Phase E.3 - Restart Semantics:
 * - E.3.1: Mining does NOT auto-resume after restart
 * - E.3.2: Mining requires explicit mining.start call
 * - E.3.3: Configuration persists, state does not
 *
 * Architecture:
 * - Tests operate on POLICY VIEWS (pure state), not implementations
 * - No MiningManager (no threads, no execution)
 * - No daemon lifecycle hooks
 * - Tests inject views directly into CheckMiningResumePolicy()
 * - Fast, deterministic, impossible to weaken
 *
 * This is NOT an integration test. Integration tests come later (Phase E.4).
 */

#include <gtest/gtest.h>
#include "rpc/mining_policy.h"

using namespace dinero::rpc;

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.3.1: Mining does NOT auto-resume after restart
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Mining auto-resume MUST be blocked
 *
 * Contract: E.3.1 - No Auto-Resume
 *
 * If this test fails, it means:
 * - The restart check was removed from CheckMiningResumePolicy()
 * - OR Mining auto-resume was enabled
 * - OR The policy contract was weakened
 *
 * DO NOT modify this test to make it pass - fix the code instead!
 */
TEST(RestartPolicyTripwires, MiningDoesNotAutoResumeAfterRestart) {
    // Setup: Fresh daemon start, mining was active before shutdown
    RestartPolicyView restart_view{
        .is_fresh_start = true,
        .mining_was_active_before = true
    };

    // Execute: Check mining resume policy
    auto result = CheckMiningResumePolicy(restart_view);

    // TRIPWIRE: This MUST return false (not allowed)
    EXPECT_FALSE(result.allowed)
        << "CRITICAL FAILURE: Mining auto-resumed after restart!\\n"
        << "This breaks E.3.1 contract: No auto-resume.\\n"
        << "DO NOT modify this test - fix CheckMiningResumePolicy() instead!";

    EXPECT_EQ(-10, result.error_code)
        << "Wrong error code for restart rejection (should be -10)";

    EXPECT_TRUE(result.must_explicitly_restart)
        << "must_explicitly_restart flag should be set";

    EXPECT_NE(std::string::npos, result.error_message.find("auto-resume"))
        << "Error message should mention auto-resume";

    EXPECT_NE(std::string::npos, result.error_message.find("mining.start"))
        << "Error message should mention mining.start explicitly";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.3.2: Clean start (no prior mining) allowed
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Clean restart with no prior mining SHOULD succeed
 *
 * Contract: E.3.2 - Fresh Start Allowed
 *
 * This proves restart detection works correctly.
 */
TEST(RestartPolicyTripwires, CleanRestartWithNoPriorMiningAllowed) {
    // Setup: Fresh daemon start, mining was NOT active before
    RestartPolicyView restart_view{
        .is_fresh_start = true,
        .mining_was_active_before = false
    };

    // Execute: Check mining resume policy
    auto result = CheckMiningResumePolicy(restart_view);

    // Verify: Policy allows this (no prior mining to resume)
    EXPECT_TRUE(result.allowed)
        << "FAILURE: Policy rejected clean restart!\\n"
        << "Error: " << result.error_message;

    EXPECT_FALSE(result.must_explicitly_restart)
        << "must_explicitly_restart should be false for clean start";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.3.3: Runtime state unaffected by restart policy
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Runtime state (not a restart) SHOULD be unaffected
 *
 * Contract: E.3.3 - Restart Policy Only Applies at Restart
 *
 * This proves the policy only triggers during actual restarts.
 */
TEST(RestartPolicyTripwires, RuntimeStateUnaffected) {
    // Setup: NOT a fresh start (daemon still running)
    RestartPolicyView restart_view{
        .is_fresh_start = false,
        .mining_was_active_before = true
    };

    // Execute: Check mining resume policy
    auto result = CheckMiningResumePolicy(restart_view);

    // Verify: Policy allows this (not a restart scenario)
    EXPECT_TRUE(result.allowed)
        << "FAILURE: Policy incorrectly blocked runtime state!\\n"
        << "Restart policy should only apply during daemon restart.\\n"
        << "Error: " << result.error_message;

    EXPECT_FALSE(result.must_explicitly_restart)
        << "must_explicitly_restart should be false during runtime";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.3.4: Both flags false (normal operation)
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Normal operation (no restart, no prior mining) allowed
 *
 * Contract: E.3.3 - Normal Operation Unaffected
 */
TEST(RestartPolicyTripwires, NormalOperationUnaffected) {
    // Setup: NOT a restart, no prior mining
    RestartPolicyView restart_view{
        .is_fresh_start = false,
        .mining_was_active_before = false
    };

    // Execute: Check mining resume policy
    auto result = CheckMiningResumePolicy(restart_view);

    // Verify: Policy allows this (normal operation)
    EXPECT_TRUE(result.allowed)
        << "FAILURE: Policy blocked normal operation!\\n"
        << "Error: " << result.error_message;

    EXPECT_FALSE(result.must_explicitly_restart)
        << "must_explicitly_restart should be false during normal operation";
}

// ═══════════════════════════════════════════════════════════════════
// TRIPWIRE E.3.5: Restart contract - CONFIG persists, STATE does not
// ═══════════════════════════════════════════════════════════════════

/**
 * Tripwire: Restart contract guarantees
 *
 * Contract: E.3 - CONFIG persists, STATE does not
 *
 * This test documents the restart contract:
 * - Mining address DOES persist (config)
 * - Mining enabled state does NOT persist (state)
 *
 * The policy layer only enforces "do not auto-resume".
 * Actual persistence is tested in integration tests (Phase E.4).
 */
TEST(RestartPolicyTripwires, RestartContractDocumented) {
    // This test documents the restart contract
    // Policy: Prevent auto-resume
    // Execution (later): mining.info shows mining=false after restart
    // Persistence (proven in Phase D): address persists

    RestartPolicyView restart_view{
        .is_fresh_start = true,
        .mining_was_active_before = true
    };

    auto result = CheckMiningResumePolicy(restart_view);

    // The contract: Auto-resume is FORBIDDEN
    EXPECT_FALSE(result.allowed)
        << "CONTRACT VIOLATION: Auto-resume is forbidden.\\n"
        << "CONFIG (address) persists, STATE (mining enabled) does not.";

    // Error message MUST be actionable
    EXPECT_NE(std::string::npos, result.error_message.find("mining.start"))
        << "User must be told HOW to resume (call mining.start)";
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
