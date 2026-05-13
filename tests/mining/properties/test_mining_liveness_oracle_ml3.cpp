#include "mining_liveness_oracle_ml3.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4e: ML3 Liveness Property Tests
// Test ML3: Blocks Eventually Submitted

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
// Test 1: Normal Mining - Solutions Submitted
// ============================================================================

void test_ml3_normal_mining_solutions_submitted() {
    ConsensusParams params = ConsensusParams::regtest();
    ML3Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML3 property
    // Normal mining submits solutions - no violations
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML3: Normal mining - solutions submitted");
}

// ============================================================================
// Test 2: Multiple Solutions - Each Submitted
// ============================================================================

void test_ml3_multiple_solutions_submitted() {
    ConsensusParams params = ConsensusParams::regtest();
    ML3Oracle oracle(params);

    MiningSimulator simulator(23456);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Mine long enough to find multiple solutions
    // Simulator auto-submits each solution immediately
    for (int i = 0; i < 15000; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator.applyAction(time_adv);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML3 property
    // Each solution should be submitted
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML3: Multiple solutions - each submitted");
}

// ============================================================================
// Test 3: Quick Solution - Immediate Submission
// ============================================================================

void test_ml3_quick_solution_immediate_submission() {
    ConsensusParams params = ConsensusParams::regtest();
    ML3Oracle oracle(params);

    MiningSimulator simulator(34567);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Mine until solution found
    for (int i = 0; i < 3000; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator.applyAction(time_adv);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML3 property
    // Solution should be submitted immediately (no delay)
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML3: Quick solution - immediate submission");
}

// ============================================================================
// Test 4: Mining After Restart - Solutions Submitted
// ============================================================================

void test_ml3_mining_after_restart_submitted() {
    ConsensusParams params = ConsensusParams::regtest();
    ML3Oracle oracle(params);

    // Generate restart scenario
    MiningSequenceGenerator generator(45678);
    MiningSimulator simulator(45678);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML3 property
    // Mining after restart should submit solutions
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML3: Mining after restart - solutions submitted");
}

// ============================================================================
// Test 5: Multiple Submissions - Forward Progress
// ============================================================================

void test_ml3_multiple_submissions_progress() {
    ConsensusParams params = ConsensusParams::regtest();
    ML3Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Mine long enough to find and submit multiple blocks
    for (int i = 0; i < 20000; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator.applyAction(time_adv);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML3 property
    // Multiple submissions show forward progress
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML3: Multiple submissions - forward progress");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ml3_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    ML3Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<LivenessViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "ML3: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<LivenessViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "ML3: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4e: ML3 Liveness Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ml3_normal_mining_solutions_submitted();
    test_ml3_multiple_solutions_submitted();
    test_ml3_quick_solution_immediate_submission();
    test_ml3_mining_after_restart_submitted();
    test_ml3_multiple_submissions_progress();
    test_ml3_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All ML3 tests passed ===" << std::endl;

    return 0;
}
