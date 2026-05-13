#include "mining_liveness_oracle_ml1.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4e: ML1 Liveness Property Tests
// Test ML1: Templates Eventually Created

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
// Test 1: Normal Mining - Templates Created
// ============================================================================

void test_ml1_normal_mining_templates_created() {
    ConsensusParams params = ConsensusParams::regtest();
    ML1Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML1 property
    // Normal mining creates templates - no violations
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML1: Normal mining - templates created");
}

// ============================================================================
// Test 2: Start/Stop Cycles - Each Creates Template
// ============================================================================

void test_ml1_start_stop_cycles_templates() {
    ConsensusParams params = ConsensusParams::regtest();
    ML1Oracle oracle(params);

    MiningSimulator simulator(23456);

    // Multiple start/stop cycles
    for (int i = 0; i < 3; i++) {
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = i * 1000;
        simulator.applyAction(start);

        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 1000 + 100;
        simulator.applyAction(time_adv);

        MiningAction stop;
        stop.type = MiningActionType::STOP_MINING;
        stop.timestamp = i * 1000 + 200;
        simulator.applyAction(stop);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML1 property
    // Each cycle should create template
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML1: Start/stop cycles - templates created");
}

// ============================================================================
// Test 3: Quick Start/Stop - No Violation
// ============================================================================

void test_ml1_quick_start_stop_no_violation() {
    ConsensusParams params = ConsensusParams::regtest();
    ML1Oracle oracle(params);

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

    // Check ML1 property
    // Quick stop before threshold - no violation
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML1: Quick start/stop - no violation");
}

// ============================================================================
// Test 4: Mining After Restart - Templates Created
// ============================================================================

void test_ml1_mining_after_restart_templates() {
    ConsensusParams params = ConsensusParams::regtest();
    ML1Oracle oracle(params);

    // Generate restart scenario
    MiningSequenceGenerator generator(45678);
    MiningSimulator simulator(45678);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML1 property
    // Mining after restart should create templates
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML1: Mining after restart - templates created");
}

// ============================================================================
// Test 5: Multiple Templates - Forward Progress
// ============================================================================

void test_ml1_multiple_templates_progress() {
    ConsensusParams params = ConsensusParams::regtest();
    ML1Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Create multiple templates
    for (int i = 0; i < 5; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 100;
        simulator.applyAction(time_adv);

        // New block arrives (template discarded, new one created)
        MiningAction new_block;
        new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
        new_block.timestamp = i * 100 + 50;
        new_block.block_hash = 0x1000 + i;
        new_block.new_height = 100 + i + 1;
        simulator.applyAction(new_block);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML1 property
    // Multiple templates show forward progress
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML1: Multiple templates - forward progress");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ml1_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    ML1Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<LivenessViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "ML1: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<LivenessViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "ML1: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4e: ML1 Liveness Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ml1_normal_mining_templates_created();
    test_ml1_start_stop_cycles_templates();
    test_ml1_quick_start_stop_no_violation();
    test_ml1_mining_after_restart_templates();
    test_ml1_multiple_templates_progress();
    test_ml1_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All ML1 tests passed ===" << std::endl;

    return 0;
}
