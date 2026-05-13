#include "mining_determinism_oracle_md2.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4f: MD2 Determinism Property Tests
// Test MD2: Restart Replay Determinism

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
// Test 1: Restart Replay is Deterministic
// ============================================================================

void test_md2_restart_replay_deterministic() {
    MD2Oracle oracle;

    const uint64_t seed = 777;

    // Run restart scenario twice
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD2 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD2: Restart replay is deterministic");
}

// ============================================================================
// Test 2: Trace Length Mismatch is Detected
// ============================================================================

void test_md2_length_mismatch_detected() {
    MD2Oracle oracle;

    // Create two traces with different lengths
    MiningSimulator simulator1(555);
    MiningSequenceGenerator generator1(555);

    std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Second trace with fewer actions
    MiningSimulator simulator2(555);
    MiningSequenceGenerator generator2(555);

    std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
    // Remove last few actions to create length mismatch
    if (actions2.size() > 3) {
        actions2.resize(actions2.size() - 3);
    }

    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD2 property
    // Length mismatch should be detected
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_has_violations(violations, "MD2: Trace length mismatch is detected");
}

// ============================================================================
// Test 3: No Restart Means No Violation
// ============================================================================

void test_md2_no_restart_no_violation() {
    MD2Oracle oracle;

    const uint64_t seed = 555;

    // Run simple scenario (no restart) twice
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateSimpleScenario();
    for (const auto& action : actions1) {
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

    // Check MD2 property
    // No restart → MD2 not applicable, should pass
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD2: No restart means no violation");
}

// ============================================================================
// Test 4: Crash Scenario Replay Determinism
// ============================================================================

void test_md2_crash_scenario_determinism() {
    MD2Oracle oracle;

    const uint64_t seed = 4242;

    // Run crash scenario twice
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateCrashScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateCrashScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD2 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD2: Crash scenario replay determinism");
}

// ============================================================================
// Test 5: Multiple Restart Cycles
// ============================================================================

void test_md2_multiple_restart_cycles() {
    MD2Oracle oracle;

    const uint64_t seed = 9999;

    // Create scenario with multiple crash/restart cycles
    MiningSimulator simulator1(seed);

    // First cycle
    MiningAction start1;
    start1.type = MiningActionType::START_MINING;
    start1.timestamp = 0;
    simulator1.applyAction(start1);

    MiningAction crash1;
    crash1.type = MiningActionType::CRASH;
    crash1.timestamp = 100;
    simulator1.applyAction(crash1);

    MiningAction restart1;
    restart1.type = MiningActionType::RESTART;
    restart1.timestamp = 200;
    simulator1.applyAction(restart1);

    // Second cycle
    MiningAction start2;
    start2.type = MiningActionType::START_MINING;
    start2.timestamp = 300;
    simulator1.applyAction(start2);

    MiningAction crash2;
    crash2.type = MiningActionType::CRASH;
    crash2.timestamp = 400;
    simulator1.applyAction(crash2);

    MiningAction restart2;
    restart2.type = MiningActionType::RESTART;
    restart2.timestamp = 500;
    simulator1.applyAction(restart2);

    MiningTrace trace1 = simulator1.extractTrace();

    // Replay with same seed
    MiningSimulator simulator2(seed);

    simulator2.applyAction(start1);
    simulator2.applyAction(crash1);
    simulator2.applyAction(restart1);
    simulator2.applyAction(start2);
    simulator2.applyAction(crash2);
    simulator2.applyAction(restart2);

    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD2 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD2: Multiple restart cycles");
}

// ============================================================================
// Test 6: Empty Trace Comparison
// ============================================================================

void test_md2_empty_trace() {
    MD2Oracle oracle;

    MiningTrace trace1;
    MiningTrace trace2;

    // Empty traces should pass (no restart)
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD2: Empty trace comparison");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4f: MD2 Determinism Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_md2_restart_replay_deterministic();
    test_md2_length_mismatch_detected();
    test_md2_no_restart_no_violation();
    test_md2_crash_scenario_determinism();
    test_md2_multiple_restart_cycles();
    test_md2_empty_trace();

    std::cout << std::endl;
    std::cout << "=== All MD2 tests passed ===" << std::endl;

    return 0;
}
