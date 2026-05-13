#include "mining_liveness_oracle_ml5.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4e: ML5 Liveness Property Tests
// Test ML5: Stale Templates Eventually Discarded

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
// Test 1: Normal Mining - Templates Discarded
// ============================================================================

void test_ml5_normal_mining_templates_discarded() {
    ConsensusParams params = ConsensusParams::regtest();
    ML5Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML5 property
    // Normal mining discards stale templates - no violations
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML5: Normal mining - templates discarded");
}

// ============================================================================
// Test 2: Reorg Scenario - Templates Discarded
// ============================================================================

void test_ml5_reorg_templates_discarded() {
    ConsensusParams params = ConsensusParams::regtest();
    ML5Oracle oracle(params);

    // Generate reorg scenario
    MiningSequenceGenerator generator(23456);
    MiningSimulator simulator(23456);

    std::vector<MiningAction> actions = generator.generateReorgScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML5 property
    // Reorg scenario should discard stale templates
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML5: Reorg - templates discarded");
}

// ============================================================================
// Test 3: Multiple Tip Changes - Templates Discarded
// ============================================================================

void test_ml5_multiple_tip_changes() {
    ConsensusParams params = ConsensusParams::regtest();
    ML5Oracle oracle(params);

    MiningSimulator simulator(34567);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Multiple tip changes
    for (int i = 0; i < 5; i++) {
        // Mine for a bit
        for (int j = 0; j < 10; j++) {
            MiningAction time_adv;
            time_adv.type = MiningActionType::TIME_ADVANCED;
            time_adv.timestamp = i * 100 + j;
            simulator.applyAction(time_adv);
        }

        // New block arrives (tip changes)
        MiningAction new_block;
        new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
        new_block.timestamp = i * 100 + 50;
        new_block.block_hash = 0x1000 + i;
        new_block.new_height = 100 + i + 1;
        simulator.applyAction(new_block);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML5 property
    // Each tip change should lead to template discard
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML5: Multiple tip changes - templates discarded");
}

// ============================================================================
// Test 4: Mining Without Tip Changes - No Violations
// ============================================================================

void test_ml5_no_tip_changes() {
    ConsensusParams params = ConsensusParams::regtest();
    ML5Oracle oracle(params);

    MiningSimulator simulator(45678);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Mine without any tip changes
    for (int i = 0; i < 100; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator.applyAction(time_adv);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check ML5 property
    // No tip changes, so no discard required
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML5: No tip changes - no violations");
}

// ============================================================================
// Test 5: Mining Stopped After Tip Change - No Violation
// ============================================================================

void test_ml5_mining_stopped_after_tip_change() {
    ConsensusParams params = ConsensusParams::regtest();
    ML5Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Mine for a bit
    for (int i = 0; i < 10; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator.applyAction(time_adv);
    }

    // New block arrives (tip changes)
    MiningAction new_block;
    new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
    new_block.timestamp = 20;
    new_block.block_hash = 0x2000;
    new_block.new_height = 101;
    simulator.applyAction(new_block);

    // Stop mining (before threshold)
    MiningAction stop;
    stop.type = MiningActionType::STOP_MINING;
    stop.timestamp = 30;
    simulator.applyAction(stop);

    MiningTrace trace = simulator.extractTrace();

    // Check ML5 property
    // Mining stopped, so template no longer relevant
    std::vector<LivenessViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "ML5: Mining stopped after tip change - no violation");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ml5_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    ML5Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateReorgScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<LivenessViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "ML5: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateReorgScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<LivenessViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "ML5: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4e: ML5 Liveness Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ml5_normal_mining_templates_discarded();
    test_ml5_reorg_templates_discarded();
    test_ml5_multiple_tip_changes();
    test_ml5_no_tip_changes();
    test_ml5_mining_stopped_after_tip_change();
    test_ml5_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All ML5 tests passed ===" << std::endl;

    return 0;
}
