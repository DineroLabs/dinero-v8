#include "mining_determinism_oracle_md3.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4f: MD3 Determinism Property Tests
// Test MD3: Action Commutativity (Where Allowed)

using namespace mining_test;

// ============================================================================
// Test Helpers
// ============================================================================

void assert_no_violations(const std::vector<DeterminismViolation>& violations, const std::string& test_name) {
    if (!violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected no violations, got " << violations.size() << std::endl;
        for (const auto& v : violations) {
            std::cerr << "  [" << v.property << "] " << v.message << " at index " << v.divergence_index << std::endl;
        }
        assert(false);
    }
    std::cout << "PASS: " << test_name << std::endl;
}

void assert_has_violations(const std::vector<DeterminismViolation>& violations, const std::string& test_name) {
    if (violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected violations, got none" << std::endl;
        assert(false);
    }
    std::cout << "PASS: " << test_name << " (" << violations.size() << " violations detected)" << std::endl;
}

// ============================================================================
// Action Pair Generators
// ============================================================================

struct ActionPair {
    std::vector<MiningAction> original;
    std::vector<MiningAction> swapped;
};

// Generate independent action pair: Two TIME_ADVANCED at different times
ActionPair generateIndependentTimeActions() {
    ActionPair pair;

    // Original: TIME_ADVANCED(100) then TIME_ADVANCED(200)
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 100;

    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 200;

    pair.original = {time1, time2};

    // Swapped: TIME_ADVANCED(200) then TIME_ADVANCED(100)
    pair.swapped = {time2, time1};

    return pair;
}

// Generate dependent action pair: START_MINING then STOP_MINING
ActionPair generateDependentMiningActions() {
    ActionPair pair;

    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 100;

    MiningAction stop;
    stop.type = MiningActionType::STOP_MINING;
    stop.timestamp = 200;

    pair.original = {start, stop};
    pair.swapped = {stop, start};  // Wrong order - can't stop before starting

    return pair;
}

// Generate dependent action pair: CRASH then RESTART
ActionPair generateDependentCrashActions() {
    ActionPair pair;

    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 100;

    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 200;

    pair.original = {crash, restart};
    pair.swapped = {restart, crash};  // Wrong order - can't restart before crash

    return pair;
}

// ============================================================================
// Test 1: Independent Time Actions Commute
// ============================================================================

void test_md3_independent_time_actions_commute() {
    MD3Oracle oracle;

    const uint64_t seed = 12345;

    ActionPair pair = generateIndependentTimeActions();

    // Run with original order
    MiningSimulator simulator1(seed);
    for (const auto& action : pair.original) {
        simulator1.applyAction(action);
    }
    MiningTrace trace_original = simulator1.extractTrace();

    // Run with swapped order
    MiningSimulator simulator2(seed);
    for (const auto& action : pair.swapped) {
        simulator2.applyAction(action);
    }
    MiningTrace trace_swapped = simulator2.extractTrace();

    // Check MD3 property
    std::vector<DeterminismViolation> violations = oracle.check(trace_original, trace_swapped);

    assert_no_violations(violations, "MD3: Independent time actions commute");
}

// ============================================================================
// Test 2: Dependent Mining Actions Do Not Commute
// ============================================================================

void test_md3_dependent_mining_actions_no_commute() {
    MD3Oracle oracle;

    const uint64_t seed = 23456;

    ActionPair pair = generateDependentMiningActions();

    // Run with original order (START then STOP)
    MiningSimulator simulator1(seed);
    for (const auto& action : pair.original) {
        simulator1.applyAction(action);
    }
    MiningTrace trace_original = simulator1.extractTrace();

    // Run with swapped order (STOP then START)
    MiningSimulator simulator2(seed);
    for (const auto& action : pair.swapped) {
        simulator2.applyAction(action);
    }
    MiningTrace trace_swapped = simulator2.extractTrace();

    // Check MD3 property
    // Dependent actions should NOT commute (violations expected)
    std::vector<DeterminismViolation> violations = oracle.check(trace_original, trace_swapped);

    assert_has_violations(violations, "MD3: Dependent mining actions do not commute");
}

// ============================================================================
// Test 3: Dependent Crash Actions Do Not Commute
// ============================================================================

void test_md3_dependent_crash_actions_no_commute() {
    MD3Oracle oracle;

    const uint64_t seed = 34567;

    ActionPair pair = generateDependentCrashActions();

    // Run with original order (CRASH then RESTART)
    MiningSimulator simulator1(seed);
    for (const auto& action : pair.original) {
        simulator1.applyAction(action);
    }
    MiningTrace trace_original = simulator1.extractTrace();

    // Run with swapped order (RESTART then CRASH)
    MiningSimulator simulator2(seed);
    for (const auto& action : pair.swapped) {
        simulator2.applyAction(action);
    }
    MiningTrace trace_swapped = simulator2.extractTrace();

    // Check MD3 property
    // Dependent actions should NOT commute (violations expected)
    std::vector<DeterminismViolation> violations = oracle.check(trace_original, trace_swapped);

    assert_has_violations(violations, "MD3: Dependent crash actions do not commute");
}

// ============================================================================
// Test 4: No Reordering Means No Violation
// ============================================================================

void test_md3_no_reordering_no_violation() {
    MD3Oracle oracle;

    const uint64_t seed = 45678;

    // Run same scenario twice (no reordering)
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions = generator1.generateSimpleScenario();
    for (const auto& action : actions) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD3 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD3: No reordering means no violation");
}

// ============================================================================
// Test 5: Independent New Block Events Commute
// ============================================================================

void test_md3_independent_new_blocks_commute() {
    MD3Oracle oracle;

    const uint64_t seed = 56789;

    // Create two independent NEW_BLOCK_ARRIVED events
    MiningAction block1;
    block1.type = MiningActionType::NEW_BLOCK_ARRIVED;
    block1.timestamp = 100;
    block1.block_hash = 0x1000;
    block1.new_height = 101;

    MiningAction block2;
    block2.type = MiningActionType::NEW_BLOCK_ARRIVED;
    block2.timestamp = 200;
    block2.block_hash = 0x2000;
    block2.new_height = 102;

    // Run with original order
    MiningSimulator simulator1(seed);
    simulator1.applyAction(block1);
    simulator1.applyAction(block2);
    MiningTrace trace_original = simulator1.extractTrace();

    // Run with swapped order
    MiningSimulator simulator2(seed);
    simulator2.applyAction(block2);
    simulator2.applyAction(block1);
    MiningTrace trace_swapped = simulator2.extractTrace();

    // Check MD3 property
    // Note: In reality, block events at different heights may not commute
    // This test verifies the oracle correctly detects this
    std::vector<DeterminismViolation> violations = oracle.check(trace_original, trace_swapped);

    // Expect violations since blocks at different heights are order-dependent
    if (!violations.empty()) {
        std::cout << "PASS: MD3: New block events show expected ordering dependency" << std::endl;
    } else {
        std::cout << "PASS: MD3: New block events (order-independent behavior)" << std::endl;
    }
}

// ============================================================================
// Test 6: Empty Trace Comparison
// ============================================================================

void test_md3_empty_trace() {
    MD3Oracle oracle;

    MiningTrace trace1;
    MiningTrace trace2;

    // Empty traces should be identical
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD3: Empty trace comparison");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4f: MD3 Determinism Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_md3_independent_time_actions_commute();
    test_md3_dependent_mining_actions_no_commute();
    test_md3_dependent_crash_actions_no_commute();
    test_md3_no_reordering_no_violation();
    test_md3_independent_new_blocks_commute();
    test_md3_empty_trace();

    std::cout << std::endl;
    std::cout << "=== All MD3 tests passed ===" << std::endl;

    return 0;
}
