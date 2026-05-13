#include "mining_safety_oracle_ms3.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4d: MS3 Safety Property Tests
// Test MS3: No Invalid Transaction Inclusion (Placeholder)

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
// Test 1: Normal Templates - Valid Transaction Counts
// ============================================================================

void test_ms3_normal_templates_valid_tx_counts() {
    ConsensusParams params = ConsensusParams::regtest();
    MS3Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS3 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS3: Normal templates - valid tx counts");
}

// ============================================================================
// Test 2: Mempool Churn - Transaction Tracking
// ============================================================================

void test_ms3_mempool_churn_tracking() {
    ConsensusParams params = ConsensusParams::regtest();
    MS3Oracle oracle(params);

    // Generate mempool churn scenario
    MiningSequenceGenerator generator(23456);
    MiningSimulator simulator(23456);

    std::vector<MiningAction> actions = generator.generateMempoolChurnScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS3 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS3: Mempool churn - transaction tracking");
}

// ============================================================================
// Test 3: Transaction Count Validation
// ============================================================================

void test_ms3_transaction_count_validation() {
    ConsensusParams params = ConsensusParams::regtest();
    MS3Oracle oracle(params);

    MiningSimulator simulator(34567);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Time advances (template created)
    MiningAction time_adv;
    time_adv.type = MiningActionType::TIME_ADVANCED;
    time_adv.timestamp = 1000;
    simulator.applyAction(time_adv);

    MiningTrace trace = simulator.extractTrace();

    // Check MS3 property
    // Phase 4b simulator creates templates with placeholder tx counts
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS3: Transaction count validation");
}

// ============================================================================
// Test 4: Template After Crash - Validation Still Runs
// ============================================================================

void test_ms3_template_after_crash_validation() {
    ConsensusParams params = ConsensusParams::regtest();
    MS3Oracle oracle(params);

    MiningSimulator simulator(45678);

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

    // Start mining again
    simulator.applyAction(start);

    // Time advances (new template)
    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 4000;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MS3 property
    // Templates created after crash should still be validated
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS3: Template after crash - validation runs");
}

// ============================================================================
// Test 5: Multiple Templates - Consistent Transaction Counts
// ============================================================================

void test_ms3_multiple_templates_consistent_counts() {
    ConsensusParams params = ConsensusParams::regtest();
    MS3Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Create multiple templates
    for (int i = 0; i < 3; i++) {
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = i * 1000;
        simulator.applyAction(start);

        // Time advances (template created)
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 1000 + 500;
        simulator.applyAction(time_adv);

        // Simulate block found (tip advances)
        MiningAction new_block;
        new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
        new_block.timestamp = i * 1000 + 600;
        new_block.block_hash = 0x3000 + i;
        new_block.new_height = 100 + i + 1;
        simulator.applyAction(new_block);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS3 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS3: Multiple templates - consistent counts");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ms3_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    MS3Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<SafetyViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "MS3: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<SafetyViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "MS3: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4d: MS3 Safety Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ms3_normal_templates_valid_tx_counts();
    test_ms3_mempool_churn_tracking();
    test_ms3_transaction_count_validation();
    test_ms3_template_after_crash_validation();
    test_ms3_multiple_templates_consistent_counts();
    test_ms3_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MS3 tests passed ===" << std::endl;

    return 0;
}
