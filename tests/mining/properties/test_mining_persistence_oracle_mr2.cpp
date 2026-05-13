#include "mining_persistence_oracle_mr2.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4g.2: MR2 Persistence Property Tests
// Test MR2: No State Duplication After Crash

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
// Test 1: Persist → Crash → Restart (No Duplication)
// ============================================================================

void test_mr2_persist_crash_restart() {
    MR2Oracle oracle;
    DeterministicPersistenceStore store(12345);

    // Generate trace with crash/restart
    MiningSimulator simulator(12345);
    MiningSequenceGenerator generator(12345);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MR2 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR2.1: Persist → crash → restart (no duplication)");
}

// ============================================================================
// Test 2: Crash During Persist (No Duplication)
// ============================================================================

void test_mr2_crash_during_persist() {
    MR2Oracle oracle;
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

    // Crash during persist (simulated)
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

    // Check MR2 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR2.2: Crash during persist (no duplication)");
}

// ============================================================================
// Test 3: Multiple Restarts (No Cumulative Duplication)
// ============================================================================

void test_mr2_multiple_restarts() {
    MR2Oracle oracle;
    DeterministicPersistenceStore store(34567);

    MiningSimulator simulator(34567);

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

        // Crash
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

    // Check MR2 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR2.3: Multiple restarts (no cumulative duplication)");
}

// ============================================================================
// Test 4: Restart With No New Mining (No Duplication)
// ============================================================================

void test_mr2_restart_no_new_mining() {
    MR2Oracle oracle;
    DeterministicPersistenceStore store(45678);

    MiningSimulator simulator(45678);

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

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 150;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    // Restart (but don't mine new blocks)
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 200;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    MiningTrace trace = simulator.extractTrace();

    // Check MR2 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR2.4: Restart with no new mining (no duplication)");
}

// ============================================================================
// Test 5: Restart + New Mining (No Duplication)
// ============================================================================

void test_mr2_restart_plus_new_mining() {
    MR2Oracle oracle;
    DeterministicPersistenceStore store(56789);

    MiningSimulator simulator(56789);

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

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 150;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 200;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    // Resume mining (new blocks)
    simulator.applyAction(start);

    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 300;
    simulator.applyAction(time2);

    MiningTrace trace = simulator.extractTrace();

    // Check MR2 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR2.5: Restart + new mining (no duplication)");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_mr2_oracle_reset() {
    MR2Oracle oracle;

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

        assert_no_violations(violations1, "MR2.6: First trace");
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

        assert_no_violations(violations2, "MR2.6: Second trace (oracle reset)");
    }

    std::cout << "PASS: MR2.6: Oracle reset between traces" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4g.2: MR2 Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_mr2_persist_crash_restart();
    test_mr2_crash_during_persist();
    test_mr2_multiple_restarts();
    test_mr2_restart_no_new_mining();
    test_mr2_restart_plus_new_mining();
    test_mr2_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MR2 tests passed ===" << std::endl;
    std::cout << "MR2: No State Duplication After Crash ✅" << std::endl;

    return 0;
}
