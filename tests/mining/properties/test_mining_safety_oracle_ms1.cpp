#include "mining_safety_oracle_ms1.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4d: MS1 Safety Property Tests
// Test MS1: No Inflation Under Restart

using namespace mining_test;

// ============================================================================
// Test Helpers
// ============================================================================

void assert_no_violations(const std::vector<SafetyViolation>& violations, const std::string& test_name) {
    if (!violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected no violations, got " << violations.size() << std::endl;
        for (const auto& v : violations) {
            std::cerr << "  [" << v.property << "] " << v.message << " at event " << v.at_event << std::endl;
        }
        assert(false);
    }
    std::cout << "PASS: " << test_name << std::endl;
}

void assert_has_violations(const std::vector<SafetyViolation>& violations, const std::string& test_name) {
    if (violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected violations, got none" << std::endl;
        assert(false);
    }
    std::cout << "PASS: " << test_name << " (" << violations.size() << " violations detected)" << std::endl;
}

// ============================================================================
// Test 1: Normal Mining - No Inflation
// ============================================================================

void test_ms1_normal_mining_no_inflation() {
    ConsensusParams params = ConsensusParams::regtest();
    MS1Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS1 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS1: Normal mining - no inflation");
}

// ============================================================================
// Test 2: Single Restart - Subsidy Preserved
// ============================================================================

void test_ms1_single_restart_subsidy_preserved() {
    ConsensusParams params = ConsensusParams::regtest();
    MS1Oracle oracle(params);

    // Generate mining with restart
    MiningSequenceGenerator generator(23456);
    MiningSimulator simulator(23456);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS1 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS1: Single restart - subsidy preserved");
}

// ============================================================================
// Test 3: Multiple Restarts - No Cumulative Inflation
// ============================================================================

void test_ms1_multiple_restarts_no_inflation() {
    ConsensusParams params = ConsensusParams::regtest();
    MS1Oracle oracle(params);

    MiningSimulator simulator(34567);

    // Create multiple crash/restart cycles manually
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;

    for (int cycle = 0; cycle < 3; cycle++) {
        // Start mining
        simulator.applyAction(start);

        // Advance time
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = cycle * 100 + 10;
        simulator.applyAction(time_adv);

        // Crash
        MiningAction crash;
        crash.type = MiningActionType::CRASH;
        crash.timestamp = cycle * 100 + 50;
        crash.description = "System crashed";
        simulator.applyAction(crash);

        // Restart
        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = cycle * 100 + 60;
        restart.description = "System restarted";
        simulator.applyAction(restart);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS1 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS1: Multiple restarts - no cumulative inflation");
}

// ============================================================================
// Test 4: Template Recreation After Restart
// ============================================================================

void test_ms1_template_recreation_after_restart() {
    ConsensusParams params = ConsensusParams::regtest();
    MS1Oracle oracle(params);

    MiningSimulator simulator(45678);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    start.description = "Start mining";
    simulator.applyAction(start);

    // Time advances (template created)
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 1000;
    simulator.applyAction(time1);

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 2000;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 3000;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    // Start mining again (template recreated)
    simulator.applyAction(start);

    // Time advances (new template created at same height)
    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 4000;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MS1 property
    // Template recreation is OK - same height can have multiple templates
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS1: Template recreation after restart");
}

// ============================================================================
// Test 5: Subsidy Consistency Across Restart
// ============================================================================

void test_ms1_subsidy_consistency_across_restart() {
    ConsensusParams params = ConsensusParams::regtest();
    MS1Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Advance time to create template
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 100;
    simulator.applyAction(time1);

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 200;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 300;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    // Resume mining
    simulator.applyAction(start);

    // Advance time (new template)
    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 400;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Verify both templates claim same subsidy
    uint64_t first_subsidy = 0;
    uint64_t second_subsidy = 0;
    int template_count = 0;

    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            if (template_count == 0 && event.subsidy_claimed.has_value()) {
                first_subsidy = *event.subsidy_claimed;
            } else if (template_count == 1 && event.subsidy_claimed.has_value()) {
                second_subsidy = *event.subsidy_claimed;
            }
            template_count++;
        }
    }

    // Both should claim same subsidy (100 DIN for regtest at height 1)
    assert(first_subsidy == second_subsidy);

    // Check MS1 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS1: Subsidy consistency across restart");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ms1_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    MS1Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<SafetyViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "MS1: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<SafetyViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "MS1: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4d: MS1 Safety Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ms1_normal_mining_no_inflation();
    test_ms1_single_restart_subsidy_preserved();
    test_ms1_multiple_restarts_no_inflation();
    test_ms1_template_recreation_after_restart();
    test_ms1_subsidy_consistency_across_restart();
    test_ms1_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MS1 tests passed ===" << std::endl;

    return 0;
}
