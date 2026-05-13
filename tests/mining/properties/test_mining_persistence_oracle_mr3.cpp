#include "mining_persistence_oracle_mr3.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4g.2: MR3 Persistence Property Tests
// Test MR3: Partial Persistence Recovers Safely

using namespace mining_test;

// ============================================================================
// Test Helpers
// ============================================================================

void assert_no_violations(
    const std::vector<PersistenceViolation>& violations,
    const std::string& test_name
) {
    if (!violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected no violations, got "
                  << violations.size() << std::endl;
        for (const auto& v : violations) {
            std::cerr << "  [" << v.property << "] " << v.message
                      << " at event " << v.event_index << std::endl;
        }
        assert(false);
    }
    std::cout << "PASS: " << test_name << std::endl;
}

void assert_has_violations(
    const std::vector<PersistenceViolation>& violations,
    const std::string& test_name
) {
    if (violations.empty()) {
        std::cerr << "FAIL: " << test_name << " - Expected violations, got none"
                  << std::endl;
        assert(false);
    }
    std::cout << "PASS: " << test_name << " (" << violations.size()
              << " violations detected)" << std::endl;
}

// ============================================================================
// Test 1: Partial Write Recovery
// ============================================================================

void test_mr3_partial_write() {
    MR3Oracle oracle;
    DeterministicPersistenceStore store(12345);

    // Generate trace with crash
    MiningSimulator simulator(12345);
    MiningSequenceGenerator generator(12345);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MR3 property (oracle will inject partial write internally)
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR3.1: Partial write recovery");
}

// ============================================================================
// Test 2: Corrupt Snapshot Recovery
// ============================================================================

void test_mr3_corrupt_snapshot() {
    MR3Oracle oracle;
    DeterministicPersistenceStore store(23456);

    MiningSimulator simulator(23456);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Advance time
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 100;
    simulator.applyAction(time1);

    // Crash (oracle will inject corruption)
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 150;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    MiningTrace trace = simulator.extractTrace();

    // Check MR3 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR3.2: Corrupt snapshot recovery");
}

// ============================================================================
// Test 3: Crash During Persist
// ============================================================================

void test_mr3_crash_during_persist() {
    MR3Oracle oracle;
    DeterministicPersistenceStore store(34567);

    MiningSimulator simulator(34567);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Advance time
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 100;
    simulator.applyAction(time1);

    // Crash during persist (simulated via fault injection)
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 150;
    crash.description = "Crash during persist";
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 200;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    MiningTrace trace = simulator.extractTrace();

    // Check MR3 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR3.3: Crash during persist");
}

// ============================================================================
// Test 4: Multiple Partial Writes
// ============================================================================

void test_mr3_multiple_partial_writes() {
    MR3Oracle oracle;
    DeterministicPersistenceStore store(45678);

    MiningSimulator simulator(45678);

    // Create multiple crash/restart cycles
    for (int cycle = 0; cycle < 3; cycle++) {
        // Start mining
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = cycle * 100;
        simulator.applyAction(start);

        // Advance time
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = cycle * 100 + 10;
        simulator.applyAction(time_adv);

        // Crash (oracle will inject faults)
        MiningAction crash;
        crash.type = MiningActionType::CRASH;
        crash.timestamp = cycle * 100 + 50;
        crash.description = "System crashed";
        simulator.applyAction(crash);

        // Restart
        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = cycle * 100 + 60;
        restart.description = "System restarted";
        simulator.applyAction(restart);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MR3 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR3.4: Multiple partial writes");
}

// ============================================================================
// Test 5: Recovery After Corruption
// ============================================================================

void test_mr3_recovery_after_corruption() {
    MR3Oracle oracle;
    DeterministicPersistenceStore store(56789);

    MiningSimulator simulator(56789);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Advance time (creates state)
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 100;
    simulator.applyAction(time1);

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 150;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    // Restart (after corruption)
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 200;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    MiningTrace trace = simulator.extractTrace();

    // Check MR3 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR3.5: Recovery after corruption");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_mr3_oracle_reset() {
    MR3Oracle oracle;

    // First trace
    {
        DeterministicPersistenceStore store1(67890);
        MiningSimulator simulator1(67890);
        MiningSequenceGenerator generator1(67890);

        std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
        for (const auto& action : actions1) {
            simulator1.applyAction(action);
        }

        MiningTrace trace1 = simulator1.extractTrace();
        std::vector<PersistenceViolation> violations1 = oracle.check(trace1, store1);

        assert_no_violations(violations1, "MR3.6: First trace");
    }

    // Second trace (oracle should handle independently)
    {
        DeterministicPersistenceStore store2(78901);
        MiningSimulator simulator2(78901);
        MiningSequenceGenerator generator2(78901);

        std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
        for (const auto& action : actions2) {
            simulator2.applyAction(action);
        }

        MiningTrace trace2 = simulator2.extractTrace();
        std::vector<PersistenceViolation> violations2 = oracle.check(trace2, store2);

        assert_no_violations(violations2, "MR3.6: Second trace (oracle reset)");
    }

    std::cout << "PASS: MR3.6: Oracle reset between traces" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4g.2: MR3 Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_mr3_partial_write();
    test_mr3_corrupt_snapshot();
    test_mr3_crash_during_persist();
    test_mr3_multiple_partial_writes();
    test_mr3_recovery_after_corruption();
    test_mr3_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MR3 tests passed ===" << std::endl;
    std::cout << "MR3: Partial Persistence Recovers Safely ✅" << std::endl;

    return 0;
}
