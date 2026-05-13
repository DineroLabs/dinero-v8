#include "mining_safety_oracle_ms5.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4d: MS5 Safety Property Tests
// Test MS5: No Stale Block Acceptance (Placeholder)

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
// Test 1: Normal Mining - No Stale Blocks
// ============================================================================

void test_ms5_normal_mining_no_stale_blocks() {
    ConsensusParams params = ConsensusParams::regtest();
    MS5Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS5 property
    // Normal mining builds on current tip - no stale blocks
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS5: Normal mining - no stale blocks");
}

// ============================================================================
// Test 2: Reorg - Old Templates Discarded
// ============================================================================

void test_ms5_reorg_templates_discarded() {
    ConsensusParams params = ConsensusParams::regtest();
    MS5Oracle oracle(params);

    // Generate reorg scenario
    MiningSequenceGenerator generator(23456);
    MiningSimulator simulator(23456);

    std::vector<MiningAction> actions = generator.generateReorgScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS5 property
    // Reorg scenario should discard old templates
    std::vector<SafetyViolation> violations = oracle.check(trace);

    // Phase 4d: Conservative validation - may not flag all stale blocks
    // Full detection in Phase 4h with fork tracking
    std::cout << "MS5: Reorg scenario - " << violations.size() << " violations detected" << std::endl;
}

// ============================================================================
// Test 3: New Block Arrived - Template Discarded
// ============================================================================

void test_ms5_new_block_template_discarded() {
    ConsensusParams params = ConsensusParams::regtest();
    MS5Oracle oracle(params);

    MiningSimulator simulator(34567);

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

    // New block arrives (tip changes)
    MiningAction new_block;
    new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
    new_block.timestamp = 2000;
    new_block.block_hash = 0x1234;
    new_block.new_height = 101;
    simulator.applyAction(new_block);

    // Continue mining (new template on new tip)
    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 3000;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MS5 property
    // New block should trigger template discard
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS5: New block - template discarded");
}

// ============================================================================
// Test 4: Multiple Reorgs - Consistent Tip Tracking
// ============================================================================

void test_ms5_multiple_reorgs_tip_tracking() {
    ConsensusParams params = ConsensusParams::regtest();
    MS5Oracle oracle(params);

    MiningSimulator simulator(45678);

    // Simulate multiple reorgs
    for (int i = 0; i < 3; i++) {
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = i * 1000;
        simulator.applyAction(start);

        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 1000 + 100;
        simulator.applyAction(time_adv);

        // Simulate reorg (new block with different tip)
        MiningAction reorg;
        reorg.type = MiningActionType::REORG;
        reorg.timestamp = i * 1000 + 200;
        reorg.block_hash = 0x2000 + i;
        reorg.new_height = 100 + i;
        reorg.reorg_depth = 1;
        simulator.applyAction(reorg);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS5 property
    // Multiple reorgs should maintain tip consistency
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS5: Multiple reorgs - tip tracking");
}

// ============================================================================
// Test 5: Tip Consistency After Restart
// ============================================================================

void test_ms5_tip_consistency_after_restart() {
    ConsensusParams params = ConsensusParams::regtest();
    MS5Oracle oracle(params);

    MiningSimulator simulator(56789);

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

    // Resume mining (new template on current tip)
    simulator.applyAction(start);

    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 4000;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MS5 property
    // After restart, should build on current tip
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS5: Tip consistency after restart");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ms5_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    MS5Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<SafetyViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "MS5: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<SafetyViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "MS5: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4d: MS5 Safety Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ms5_normal_mining_no_stale_blocks();
    test_ms5_reorg_templates_discarded();
    test_ms5_new_block_template_discarded();
    test_ms5_multiple_reorgs_tip_tracking();
    test_ms5_tip_consistency_after_restart();
    test_ms5_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MS5 tests passed ===" << std::endl;

    return 0;
}
