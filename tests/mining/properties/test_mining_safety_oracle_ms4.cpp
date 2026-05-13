#include "mining_safety_oracle_ms4.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4d: MS4 Safety Property Tests
// Test MS4: Consensus Always Enforced (Placeholder)

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
// Test 1: Normal Mining - Validation Steps Occur
// ============================================================================

void test_ms4_normal_mining_validation_occurs() {
    ConsensusParams params = ConsensusParams::regtest();
    MS4Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS4 property
    // Normal mining follows proper validation steps
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS4: Normal mining - validation steps occur");
}

// ============================================================================
// Test 2: Crash Prevents Validation - No Bypass
// ============================================================================

void test_ms4_crash_prevents_validation_bypass() {
    ConsensusParams params = ConsensusParams::regtest();
    MS4Oracle oracle(params);

    MiningSimulator simulator(23456);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
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

    // Resume mining (new template after restart)
    simulator.applyAction(start);

    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 4000;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MS4 property
    // No violations - templates created after restart
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS4: Crash/restart - no validation bypass");
}

// ============================================================================
// Test 3: Restart Does Not Bypass Validation Requirement
// ============================================================================

void test_ms4_restart_requires_validation() {
    ConsensusParams params = ConsensusParams::regtest();
    MS4Oracle oracle(params);

    // Generate restart scenario
    MiningSequenceGenerator generator(34567);
    MiningSimulator simulator(34567);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS4 property
    // Restart scenario should follow proper validation steps
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS4: Restart requires validation");
}

// ============================================================================
// Test 4: Multiple Restarts - Validation Always Enforced
// ============================================================================

void test_ms4_multiple_restarts_validation_enforced() {
    ConsensusParams params = ConsensusParams::regtest();
    MS4Oracle oracle(params);

    MiningSimulator simulator(45678);

    // Multiple restart cycles
    for (int cycle = 0; cycle < 3; cycle++) {
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = cycle * 1000;
        simulator.applyAction(start);

        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = cycle * 1000 + 100;
        simulator.applyAction(time_adv);

        MiningAction crash;
        crash.type = MiningActionType::CRASH;
        crash.timestamp = cycle * 1000 + 200;
        crash.description = "System crashed";
        simulator.applyAction(crash);

        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = cycle * 1000 + 300;
        restart.description = "System restarted";
        simulator.applyAction(restart);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS4 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS4: Multiple restarts - validation enforced");
}

// ============================================================================
// Test 5: Reorg Does Not Bypass Validation
// ============================================================================

void test_ms4_reorg_validation_enforced() {
    ConsensusParams params = ConsensusParams::regtest();
    MS4Oracle oracle(params);

    // Generate reorg scenario
    MiningSequenceGenerator generator(56789);
    MiningSimulator simulator(56789);

    std::vector<MiningAction> actions = generator.generateReorgScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS4 property
    // Reorg should not bypass validation
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS4: Reorg - validation enforced");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ms4_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    MS4Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<SafetyViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "MS4: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<SafetyViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "MS4: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4d: MS4 Safety Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ms4_normal_mining_validation_occurs();
    test_ms4_crash_prevents_validation_bypass();
    test_ms4_restart_requires_validation();
    test_ms4_multiple_restarts_validation_enforced();
    test_ms4_reorg_validation_enforced();
    test_ms4_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MS4 tests passed ===" << std::endl;

    return 0;
}
