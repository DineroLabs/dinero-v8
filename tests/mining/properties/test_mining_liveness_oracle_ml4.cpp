#include "mining_liveness_oracle_ml4.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4e: ML4 Liveness Property Tests
// Test ML4: Mining Eventually Restarts

using namespace mining_test;

// ============================================================================
// Test Helpers
// ============================================================================

void assert_no_violations(const std::vector<LivenessViolation>& violations, const std::string& test_name) {
    if (!violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected no violations, got " << violations.size() << std::endl;
        for (const auto& v : violations) {
            std::cerr << "  [" << v.property << "] " << v.message << " at event " << v.at_event << std::endl;
        }
        assert(false);
    }
    std::cout << "PASS: " << test_name << std::endl;
}

void assert_has_violations(const std::vector<LivenessViolation>& violations, const std::string& test_name) {
    if (violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected violations, got none" << std::endl;
        assert(false);
    }
    std::cout << "PASS: " << test_name << " (" << violations.size() << " violations detected)" << std::endl;
}

// ============================================================================
// Test 1: Normal Mining - No Crashes
// ============================================================================

void test_ml4_normal_mining_no_crashes() {
    ConsensusParams params = ConsensusParams::regtest();
    ML4Oracle oracle(params);

    // Generate simple mining scenario (no crashes)
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML4 property
    // No crashes, so no restart required - no violations
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML4: Normal mining - no crashes");
}

// ============================================================================
// Test 2: Crash and Restart - Mining Resumes
// ============================================================================

void test_ml4_crash_restart_mining_resumes() {
    ConsensusParams params = ConsensusParams::regtest();
    ML4Oracle oracle(params);

    // Generate restart scenario
    MiningSequenceGenerator generator(23456);
    MiningSimulator simulator(23456);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML4 property
    // System crashes and restarts, mining should resume
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML4: Crash and restart - mining resumes");
}

// ============================================================================
// Test 3: Crash Scenario - Comprehensive
// ============================================================================

void test_ml4_crash_scenario_comprehensive() {
    ConsensusParams params = ConsensusParams::regtest();
    ML4Oracle oracle(params);

    // Generate crash scenario
    MiningSequenceGenerator generator(34567);
    MiningSimulator simulator(34567);

    std::vector<MiningAction> actions = generator.generateCrashScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML4 property
    // Crash scenario should handle restarts properly
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML4: Crash scenario - comprehensive");
}

// ============================================================================
// Test 4: Multiple Crash/Restart Cycles
// ============================================================================

void test_ml4_multiple_crash_restart_cycles() {
    ConsensusParams params = ConsensusParams::regtest();
    ML4Oracle oracle(params);

    MiningSimulator simulator(45678);

    // Multiple crash/restart cycles
    for (int i = 0; i < 3; i++) {
        // Start mining
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = i * 1000;
        simulator.applyAction(start);

        // Mine for a bit
        for (int j = 0; j < 10; j++) {
            MiningAction time_adv;
            time_adv.type = MiningActionType::TIME_ADVANCED;
            time_adv.timestamp = i * 1000 + j;
            simulator.applyAction(time_adv);
        }

        // Crash
        MiningAction crash;
        crash.type = MiningActionType::CRASH;
        crash.timestamp = i * 1000 + 20;
        simulator.applyAction(crash);

        // Restart
        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = i * 1000 + 30;
        simulator.applyAction(restart);

        // Resume mining
        MiningAction resume;
        resume.type = MiningActionType::START_MINING;
        resume.timestamp = i * 1000 + 40;
        simulator.applyAction(resume);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML4 property
    // All restarts should lead to mining resumption
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML4: Multiple crash/restart cycles");
}

// ============================================================================
// Test 5: Restart With Delayed Mining Resume
// ============================================================================

void test_ml4_restart_delayed_resume() {
    ConsensusParams params = ConsensusParams::regtest();
    ML4Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 100;
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 200;
    simulator.applyAction(restart);

    // Delay before resuming (but within threshold)
    for (int i = 0; i < 50; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = 300 + i;
        simulator.applyAction(time_adv);
    }

    // Resume mining
    MiningAction resume;
    resume.type = MiningActionType::START_MINING;
    resume.timestamp = 400;
    simulator.applyAction(resume);

    MiningTrace trace = simulator.extractTrace();

    // Check ML4 property
    // Mining resumes after delay (but within threshold)
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML4: Restart with delayed resume");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ml4_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    ML4Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateRestartScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<LivenessViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "ML4: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateRestartScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<LivenessViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "ML4: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4e: ML4 Liveness Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ml4_normal_mining_no_crashes();
    test_ml4_crash_restart_mining_resumes();
    test_ml4_crash_scenario_comprehensive();
    test_ml4_multiple_crash_restart_cycles();
    test_ml4_restart_delayed_resume();
    test_ml4_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All ML4 tests passed ===" << std::endl;

    return 0;
}
