#include "mining_determinism_oracle_md5.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4f: MD5 Determinism Property Tests
// Test MD5: Deterministic Crash Recovery

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
// Test 1: Identical Crash Recovery Is Deterministic
// ============================================================================

void test_md5_identical_crash_recovery() {
    MD5Oracle oracle;

    const uint64_t seed = 12345;

    // Run crash scenario twice with same seed
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateCrashScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Second run with same seed
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateCrashScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD5 property
    // Identical crash recovery should produce identical traces
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD5: Identical crash recovery is deterministic");
}

// ============================================================================
// Test 2: Restart Scenario Has Deterministic Recovery
// ============================================================================

void test_md5_restart_deterministic_recovery() {
    MD5Oracle oracle;

    const uint64_t seed = 22222;

    // Run restart scenario twice with same seed
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Second run with same seed
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD5 property
    // Restart recovery should be deterministic
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD5: Restart scenario has deterministic recovery");
}

// ============================================================================
// Test 3: Multiple Crash-Recovery Cycles Are Deterministic
// ============================================================================

void test_md5_multiple_crash_recovery_cycles() {
    MD5Oracle oracle;

    const uint64_t seed = 33333;

    // Run scenario with multiple crash-recovery cycles
    MiningSimulator simulator1(seed);

    // First crash-recovery cycle
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

    // Second crash-recovery cycle
    MiningAction crash2;
    crash2.type = MiningActionType::CRASH;
    crash2.timestamp = 300;
    simulator1.applyAction(crash2);

    MiningAction restart2;
    restart2.type = MiningActionType::RESTART;
    restart2.timestamp = 400;
    simulator1.applyAction(restart2);

    MiningTrace trace1 = simulator1.extractTrace();

    // Replay with same seed
    MiningSimulator simulator2(seed);
    simulator2.applyAction(start1);
    simulator2.applyAction(crash1);
    simulator2.applyAction(restart1);
    simulator2.applyAction(crash2);
    simulator2.applyAction(restart2);

    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD5 property
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD5: Multiple crash-recovery cycles are deterministic");
}

// ============================================================================
// Test 4: No Crash Scenario (Fallback To Full Trace Comparison)
// ============================================================================

void test_md5_no_crash_fallback() {
    MD5Oracle oracle;

    const uint64_t seed = 44444;

    // Run simple scenario without crash (no crash recovery)
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateSimpleScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Second run with same seed
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateSimpleScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD5 property
    // No crash recovery - should fall back to full trace comparison
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD5: No crash scenario (fallback to full comparison)");
}

// ============================================================================
// Test 5: Crash At Different Indices Produces Different Traces
// ============================================================================

void test_md5_crash_at_different_indices() {
    MD5Oracle oracle;

    const uint64_t seed = 55555;

    // First scenario: crash early
    MiningSimulator simulator1(seed);

    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator1.applyAction(start);

    MiningAction crash_early;
    crash_early.type = MiningActionType::CRASH;
    crash_early.timestamp = 50;
    simulator1.applyAction(crash_early);

    MiningAction restart_early;
    restart_early.type = MiningActionType::RESTART;
    restart_early.timestamp = 100;
    simulator1.applyAction(restart_early);

    MiningTrace trace1 = simulator1.extractTrace();

    // Second scenario: crash late
    MiningSimulator simulator2(seed);
    simulator2.applyAction(start);

    // More events before crash
    for (int i = 0; i < 10; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i * 10;
        simulator2.applyAction(time_adv);
    }

    MiningAction crash_late;
    crash_late.type = MiningActionType::CRASH;
    crash_late.timestamp = 150;
    simulator2.applyAction(crash_late);

    MiningAction restart_late;
    restart_late.type = MiningActionType::RESTART;
    restart_late.timestamp = 200;
    simulator2.applyAction(restart_late);

    MiningTrace trace2 = simulator2.extractTrace();

    // Check MD5 property
    // Crash at different indices should produce different traces
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_has_violations(violations, "MD5: Crash at different indices produces different traces");
}

// ============================================================================
// Test 6: Empty Trace Comparison
// ============================================================================

void test_md5_empty_trace() {
    MD5Oracle oracle;

    MiningTrace trace1;
    MiningTrace trace2;

    // Empty traces should be identical
    std::vector<DeterminismViolation> violations = oracle.check(trace1, trace2);

    assert_no_violations(violations, "MD5: Empty trace comparison");
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4f: MD5 Determinism Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_md5_identical_crash_recovery();
    test_md5_restart_deterministic_recovery();
    test_md5_multiple_crash_recovery_cycles();
    test_md5_no_crash_fallback();
    test_md5_crash_at_different_indices();
    test_md5_empty_trace();

    std::cout << std::endl;
    std::cout << "=== All MD5 tests passed ===" << std::endl;
    std::cout << std::endl;
    std::cout << "Crash recovery is deterministic ✅" << std::endl;
    std::cout << std::endl;
    std::cout << "🏁 Phase 4f COMPLETE - All determinism properties sealed 🏁" << std::endl;

    return 0;
}
