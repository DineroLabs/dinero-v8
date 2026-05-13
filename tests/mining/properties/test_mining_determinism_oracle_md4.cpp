#include "mining_determinism_oracle_md4.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

// Ring 4 Phase 4f: MD4 Determinism Property Tests
// Test MD4: No Hidden Entropy Sources

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
// Test 1: Identical Seed With Time Delay Remains Deterministic
// ============================================================================

void test_md4_identical_seed_time_delay() {
    MD4Oracle oracle;

    const uint64_t seed = 999;

    // Run first execution
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Introduce time delay (simulates different execution environment)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Run second execution with same seed
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD4 property
    // Time delay should NOT cause divergence if no hidden entropy
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD4: Identical seed with time delay remains deterministic");
}

// ============================================================================
// Test 2: Different Seed Produces Divergence (Expected)
// ============================================================================

void test_md4_different_seed_divergence() {
    MD4Oracle oracle;

    // Run with first seed
    MiningSimulator simulator1(111);
    MiningSequenceGenerator generator1(111);

    std::vector<MiningAction> actions1 = generator1.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Run with different seed
    MiningSimulator simulator2(222);
    MiningSequenceGenerator generator2(222);

    std::vector<MiningAction> actions2 = generator2.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD4 property
    // Different seeds SHOULD produce divergence
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_has_violations(violations, "MD4: Different seed produces divergence");
}

// ============================================================================
// Test 3: Restart Scenario Has No Hidden Entropy
// ============================================================================

void test_md4_restart_no_hidden_entropy() {
    MD4Oracle oracle;

    const uint64_t seed = 424242;

    // Run restart scenario
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Small delay
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Run again with same seed
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD4 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD4: Restart scenario has no hidden entropy");
}

// ============================================================================
// Test 4: Multiple Sequential Runs (Entropy Audit)
// ============================================================================

void test_md4_multiple_sequential_runs() {
    MD4Oracle oracle;

    const uint64_t seed = 55555;

    // Generate reference trace
    MiningSimulator ref_simulator(seed);
    MiningSequenceGenerator ref_generator(seed);

    std::vector<MiningAction> ref_actions = ref_generator.generateSimpleScenario();
    for (const auto& action : ref_actions) {
        ref_simulator.applyAction(action);
    }
    MiningTrace reference = ref_simulator.extractTrace();

    // Run 10 times with delays between runs
    // If ANY hidden entropy exists, we'll catch it
    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        MiningSimulator simulator(seed);
        MiningSequenceGenerator generator(seed);

        std::vector<MiningAction> actions = generator.generateSimpleScenario();
        for (const auto& action : actions) {
            simulator.applyAction(action);
        }
        MiningTrace trace = simulator.extractTrace();

        std::vector<DeterminismViolation> violations = oracle.check(reference, trace);

        if (!violations.empty()) {
            std::cerr << "FAIL: MD4: Multiple sequential runs - Run " << i << " diverged" << std::endl;
            assert(false);
        }
    }

    std::cout << "PASS: MD4: Multiple sequential runs (10 runs, no hidden entropy)" << std::endl;
}

// ============================================================================
// Test 5: Crash Scenario Has No Hidden Entropy
// ============================================================================

void test_md4_crash_no_hidden_entropy() {
    MD4Oracle oracle;

    const uint64_t seed = 77777;

    // Run crash scenario
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateCrashScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Delay
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    // Run again
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateCrashScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD4 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD4: Crash scenario has no hidden entropy");
}

// ============================================================================
// Test 6: Complex Scenario With Mining Has No Hidden Entropy
// ============================================================================

void test_md4_complex_mining_no_hidden_entropy() {
    MD4Oracle oracle;

    const uint64_t seed = 88888;

    // Run complex scenario with mining
    MiningSimulator simulator1(seed);

    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator1.applyAction(start);

    // Mine for a while (triggers RNG)
    for (int i = 0; i < 5000; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator1.applyAction(time_adv);
    }

    MiningTrace trace1 = simulator1.extractTrace();

    // Delay
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // Run again with same seed
    MiningSimulator simulator2(seed);

    simulator2.applyAction(start);

    for (int i = 0; i < 5000; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i;
        simulator2.applyAction(time_adv);
    }

    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD4 property
    // If RNG uses any hidden entropy, this will catch it
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD4: Complex mining scenario has no hidden entropy");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4f: MD4 Determinism Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_md4_identical_seed_time_delay();
    test_md4_different_seed_divergence();
    test_md4_restart_no_hidden_entropy();
    test_md4_multiple_sequential_runs();
    test_md4_crash_no_hidden_entropy();
    test_md4_complex_mining_no_hidden_entropy();

    std::cout << std::endl;
    std::cout << "=== All MD4 tests passed ===" << std::endl;
    std::cout << std::endl;
    std::cout << "Entropy audit complete: All randomness derives from seed ✅" << std::endl;

    return 0;
}
