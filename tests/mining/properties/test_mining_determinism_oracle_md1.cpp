#include "mining_determinism_oracle_md1.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4f: MD1 Determinism Property Tests
// Test MD1: Same Seed → Identical Trace

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
// Test 1: Same Seed Produces Identical Trace
// ============================================================================

void test_md1_same_seed_identical_trace() {
    MD1Oracle oracle;

    const uint64_t seed = 12345;

    // Run twice with same seed
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Second run with identical seed
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD1 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD1: Same seed produces identical trace");
}

// ============================================================================
// Test 2: Different Seeds Produce Different Traces
// ============================================================================

void test_md1_different_seeds_different_traces() {
    MD1Oracle oracle;

    // Run with first seed
    MiningSimulator simulator1(11111);
    MiningSequenceGenerator generator1(11111);

    std::vector<MiningAction> actions1 = generator1.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Run with different seed
    MiningSimulator simulator2(22222);
    MiningSequenceGenerator generator2(22222);

    std::vector<MiningAction> actions2 = generator2.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD1 property
    // Different seeds SHOULD produce different traces (violations expected)
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_has_violations(violations, "MD1: Different seeds produce different traces");
}

// ============================================================================
// Test 3: Replay Determinism (Multiple Runs, Same Seed)
// ============================================================================

void test_md1_replay_determinism() {
    MD1Oracle oracle;

    const uint64_t seed = 42;

    // Generate reference trace
    MiningSimulator ref_simulator(seed);
    MiningSequenceGenerator ref_generator(seed);

    std::vector<MiningAction> ref_actions = ref_generator.generateSimpleScenario();
    for (const auto& action : ref_actions) {
        ref_simulator.applyAction(action);
    }
    MiningTrace reference = ref_simulator.extractTrace();

    // Replay 5 times - all should match reference
    for (int i = 0; i < 5; i++) {
        MiningSimulator replay_simulator(seed);
        MiningSequenceGenerator replay_generator(seed);

        std::vector<MiningAction> replay_actions = replay_generator.generateSimpleScenario();
        for (const auto& action : replay_actions) {
            replay_simulator.applyAction(action);
        }
        MiningTrace replay = replay_simulator.extractTrace();

        std::vector<DeterminismViolation> violations = oracle.check(reference, replay);

        if (!violations.empty()) {
            std::cerr << "FAIL: MD1: Replay determinism - Run " << i << " diverged" << std::endl;
            assert(false);
        }
    }

    std::cout << "PASS: MD1: Replay determinism (5 runs)" << std::endl;
}

// ============================================================================
// Test 4: Empty Trace Comparison
// ============================================================================

void test_md1_empty_trace_comparison() {
    MD1Oracle oracle;

    // Create two empty traces
    MiningTrace trace1;
    MiningTrace trace2;

    // Empty traces should be identical
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD1: Empty trace comparison");
}

// ============================================================================
// Test 5: Restart Scenario Determinism
// ============================================================================

void test_md1_restart_scenario_determinism() {
    MD1Oracle oracle;

    const uint64_t seed = 67890;

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

    // Check MD1 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD1: Restart scenario determinism");
}

// ============================================================================
// Test 6: Crash Scenario Determinism
// ============================================================================

void test_md1_crash_scenario_determinism() {
    MD1Oracle oracle;

    const uint64_t seed = 99999;

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

    // Check MD1 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD1: Crash scenario determinism");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4f: MD1 Determinism Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_md1_same_seed_identical_trace();
    test_md1_different_seeds_different_traces();
    test_md1_replay_determinism();
    test_md1_empty_trace_comparison();
    test_md1_restart_scenario_determinism();
    test_md1_crash_scenario_determinism();

    std::cout << std::endl;
    std::cout << "=== All MD1 tests passed ===" << std::endl;

    return 0;
}
