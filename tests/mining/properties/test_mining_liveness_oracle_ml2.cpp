#include "mining_liveness_oracle_ml2.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4e: ML2 Liveness Property Tests
// Test ML2: Solutions Eventually Found

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
// Test 1: Normal Mining - Solutions Found
// ============================================================================

void test_ml2_normal_mining_solutions_found() {
    ConsensusParams params = ConsensusParams::regtest();
    ML2Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML2 property
    // Normal mining finds solutions - no violations
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML2: Normal mining - solutions found");
}

// ============================================================================
// Test 2: Multiple Mining Cycles - Each Finds Solutions
// ============================================================================

void test_ml2_multiple_cycles_solutions() {
    ConsensusParams params = ConsensusParams::regtest();
    ML2Oracle oracle(params);

    MiningSimulator simulator(23456);

    // Multiple mining cycles, each finding a solution
    for (int i = 0; i < 3; i++) {
        // Start mining
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = i * 10000;
        simulator.applyAction(start);

        // Mine long enough to find a solution (need 1000-6000 iterations)
        // Simulator auto-generates SOLUTION_FOUND after threshold
        for (int j = 0; j < 2000; j++) {
            MiningAction time_adv;
            time_adv.type = MiningActionType::TIME_ADVANCED;
            time_adv.timestamp = i * 10000 + j;
            simulator.applyAction(time_adv);
        }

        // Stop mining
        MiningAction stop;
        stop.type = MiningActionType::STOP_MINING;
        stop.timestamp = i * 10000 + 3000;
        simulator.applyAction(stop);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML2 property
    // Each cycle should find solution
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML2: Multiple cycles - solutions found");
}

// ============================================================================
// Test 3: Quick Start/Stop - No Violation
// ============================================================================

void test_ml2_quick_start_stop_no_violation() {
    ConsensusParams params = ConsensusParams::regtest();
    ML2Oracle oracle(params);

    MiningSimulator simulator(34567);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Stop quickly (before threshold)
    MiningAction stop;
    stop.type = MiningActionType::STOP_MINING;
    stop.timestamp = 100;
    simulator.applyAction(stop);

    MiningTrace trace = simulator.extractTrace();

    // Check ML2 property
    // Quick stop before threshold - no violation
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML2: Quick start/stop - no violation");
}

// ============================================================================
// Test 4: Mining After Restart - Solutions Found
// ============================================================================

void test_ml2_mining_after_restart_solutions() {
    ConsensusParams params = ConsensusParams::regtest();
    ML2Oracle oracle(params);

    // Generate restart scenario
    MiningSequenceGenerator generator(45678);
    MiningSimulator simulator(45678);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML2 property
    // Mining after restart should find solutions
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML2: Mining after restart - solutions found");
}

// ============================================================================
// Test 5: Multiple Solutions - Forward Progress
// ============================================================================

void test_ml2_multiple_solutions_progress() {
    ConsensusParams params = ConsensusParams::regtest();
    ML2Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Mine long enough to find multiple solutions
    // Each solution auto-restarts mining with new template
    // Need enough iterations to find several solutions
    for (int i = 0; i < 15000; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator.applyAction(time_adv);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML2 property
    // Should have found multiple solutions showing forward progress
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML2: Multiple solutions - forward progress");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ml2_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    ML2Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<LivenessViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "ML2: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<LivenessViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "ML2: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4e: ML2 Liveness Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ml2_normal_mining_solutions_found();
    test_ml2_multiple_cycles_solutions();
    test_ml2_quick_start_stop_no_violation();
    test_ml2_mining_after_restart_solutions();
    test_ml2_multiple_solutions_progress();
    test_ml2_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All ML2 tests passed ===" << std::endl;

    return 0;
}
