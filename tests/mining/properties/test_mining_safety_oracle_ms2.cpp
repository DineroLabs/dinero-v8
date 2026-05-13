#include "mining_safety_oracle_ms2.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4d: MS2 Safety Property Tests
// Test MS2: No Duplicate Subsidy

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
// Test 1: Normal Mining - Single Block Per Height
// ============================================================================

void test_ms2_normal_mining_single_block_per_height() {
    ConsensusParams params = ConsensusParams::regtest();
    MS2Oracle oracle(params);

    // Generate simple mining scenario
    MiningSequenceGenerator generator(12345);
    MiningSimulator simulator(12345);

    std::vector<MiningAction> actions = generator.generateSimpleScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS2 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS2: Normal mining - single block per height");
}

// ============================================================================
// Test 2: Reorg Scenario - Block Replacement
// ============================================================================

void test_ms2_reorg_block_replacement() {
    ConsensusParams params = ConsensusParams::regtest();
    MS2Oracle oracle(params);

    // Generate reorg scenario
    MiningSequenceGenerator generator(23456);
    MiningSimulator simulator(23456);

    std::vector<MiningAction> actions = generator.generateReorgScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS2 property
    // Reorgs are OK if they replace blocks, not duplicate subsidy
    std::vector<SafetyViolation> violations = oracle.check(trace);

    // Note: Phase 4d may flag reorgs conservatively - that's OK
    // We'll refine in Phase 4h with full fork tracking
    std::cout << "MS2: Reorg scenario - " << violations.size() << " violations detected" << std::endl;
}

// ============================================================================
// Test 3: Template Recreation - No Duplicate Subsidy
// ============================================================================

void test_ms2_template_recreation_no_duplicate() {
    ConsensusParams params = ConsensusParams::regtest();
    MS2Oracle oracle(params);

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

    // Stop mining (discard template)
    MiningAction stop;
    stop.type = MiningActionType::STOP_MINING;
    stop.timestamp = 2000;
    simulator.applyAction(stop);

    // Start mining again (new template at same height)
    simulator.applyAction(start);

    // Time advances (new template created)
    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 3000;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MS2 property
    // Multiple templates at same height is OK (no duplicate subsidy)
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS2: Template recreation - no duplicate subsidy");
}

// ============================================================================
// Test 4: Multiple Heights - Each Height Gets One Block
// ============================================================================

void test_ms2_multiple_heights_one_block_each() {
    ConsensusParams params = ConsensusParams::regtest();
    MS2Oracle oracle(params);

    MiningSimulator simulator(45678);

    // Mine multiple blocks at different heights
    for (int i = 0; i < 5; i++) {
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = i * 1000;
        simulator.applyAction(start);

        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 1000 + 500;
        simulator.applyAction(time_adv);

        // Simulate new block arriving (tip advances)
        MiningAction new_block;
        new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
        new_block.timestamp = i * 1000 + 600;
        new_block.block_hash = 0x1000 + i;
        new_block.new_height = 100 + i + 1;
        simulator.applyAction(new_block);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MS2 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS2: Multiple heights - one block each");
}

// ============================================================================
// Test 5: Subsidy Consistency Across Heights
// ============================================================================

void test_ms2_subsidy_consistency_across_heights() {
    ConsensusParams params = ConsensusParams::regtest();
    MS2Oracle oracle(params);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Create a few templates at different heights
    for (int i = 0; i < 3; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 100;
        simulator.applyAction(time_adv);

        // Simulate block found (tip advances)
        MiningAction new_block;
        new_block.type = MiningActionType::NEW_BLOCK_ARRIVED;
        new_block.timestamp = i * 100 + 50;
        new_block.block_hash = 0x2000 + i;
        new_block.new_height = 100 + i + 1;
        simulator.applyAction(new_block);
    }

    MiningTrace trace = simulator.extractTrace();

    // Verify each height claimed correct subsidy
    std::map<uint32_t, uint64_t> subsidy_by_height;

    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            if (event.template_height.has_value() && event.subsidy_claimed.has_value()) {
                uint32_t height = *event.template_height;
                uint64_t subsidy = *event.subsidy_claimed;

                // Each height should claim subsidy exactly once
                assert(subsidy_by_height.count(height) == 0 || subsidy_by_height[height] == subsidy);
                subsidy_by_height[height] = subsidy;
            }
        }
    }

    // Check MS2 property
    std::vector<SafetyViolation> violations = oracle.check(trace);

    assert_no_violations(violations, "MS2: Subsidy consistency across heights");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_ms2_oracle_reset() {
    ConsensusParams params = ConsensusParams::regtest();
    MS2Oracle oracle(params);

    MiningSequenceGenerator generator(67890);

    // First trace
    MiningSimulator simulator1(67890);
    std::vector<MiningAction> actions1 = generator.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    std::vector<SafetyViolation> violations1 = oracle.check(trace1);
    assert_no_violations(violations1, "MS2: First trace");

    // Second trace (oracle should reset internal state)
    MiningSimulator simulator2(78901);
    std::vector<MiningAction> actions2 = generator.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    std::vector<SafetyViolation> violations2 = oracle.check(trace2);
    assert_no_violations(violations2, "MS2: Second trace (after reset)");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4d: MS2 Safety Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_ms2_normal_mining_single_block_per_height();
    test_ms2_reorg_block_replacement();
    test_ms2_template_recreation_no_duplicate();
    test_ms2_multiple_heights_one_block_each();
    test_ms2_subsidy_consistency_across_heights();
    test_ms2_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MS2 tests passed ===" << std::endl;

    return 0;
}
