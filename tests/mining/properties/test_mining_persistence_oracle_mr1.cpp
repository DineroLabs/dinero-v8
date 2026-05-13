#include "mining_persistence_oracle_mr1.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4g.2: MR1 Persistence Property Tests
// Test MR1: State Survives Restart Correctly

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
// Test 1: Persist → Crash → Restart
// ============================================================================

void test_mr1_persist_crash_restart() {
    MR1Oracle oracle;
    DeterministicPersistenceStore store(12345);

    // Generate trace with crash/restart
    MiningSimulator simulator(12345);
    MiningSequenceGenerator generator(12345);

    std::vector<MiningAction> actions = generator.generateRestartScenario();

    for (const auto& action : actions) {
        simulator.applyAction(action);
    }

    MiningTrace trace = simulator.extractTrace();

    // Check MR1 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR1.1: Persist → crash → restart");
}

// ============================================================================
// Test 2: Persist → Multiple Crashes → Restart
// ============================================================================

void test_mr1_multiple_crashes() {
    MR1Oracle oracle;
    DeterministicPersistenceStore store(23456);

    MiningSimulator simulator(23456);

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

    // Check MR1 property
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR1.2: Multiple crashes → restart");
}

// ============================================================================
// Test 3: Persist Twice → Crash → Restart (Latest Wins)
// ============================================================================

void test_mr1_persist_twice_latest_wins() {
    MR1Oracle oracle;
    DeterministicPersistenceStore store(34567);

    MiningSimulator simulator(34567);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    simulator.applyAction(start);

    // Advance time (creates first snapshot)
    MiningAction time1;
    time1.type = MiningActionType::TIME_ADVANCED;
    time1.timestamp = 100;
    simulator.applyAction(time1);

    // Simulate first persist (oracle will do this before crash)
    // The oracle will persist state before crash

    // Advance time again (creates second snapshot)
    MiningAction time2;
    time2.type = MiningActionType::TIME_ADVANCED;
    time2.timestamp = 200;
    simulator.applyAction(time2);

    // Crash (oracle will persist most recent state)
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 250;
    crash.description = "System crashed";
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 300;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    MiningTrace trace = simulator.extractTrace();

    // Check MR1 property
    // Latest state before crash should be recovered
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR1.3: Persist twice → crash (latest wins)");
}

// ============================================================================
// Test 4: Restart Without Persist (No-Op)
// ============================================================================

void test_mr1_restart_without_persist() {
    MR1Oracle oracle;
    DeterministicPersistenceStore store(45678);

    MiningSimulator simulator(45678);

    // Restart without prior crash or persist
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 0;
    restart.description = "Cold start";
    simulator.applyAction(restart);

    MiningTrace trace = simulator.extractTrace();

    // Check MR1 property
    // No violation - restart without persist is allowed
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR1.4: Restart without persist (no-op)");
}

// ============================================================================
// Test 5: Crash Before Persist (No-Op)
// ============================================================================

void test_mr1_crash_before_persist() {
    MR1Oracle oracle;
    DeterministicPersistenceStore store(56789);

    MiningSimulator simulator(56789);

    // Crash immediately (no state to persist)
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 0;
    crash.description = "Immediate crash";
    simulator.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 10;
    restart.description = "System restarted";
    simulator.applyAction(restart);

    MiningTrace trace = simulator.extractTrace();

    // Check MR1 property
    // No violation - crash before persist means no recovery expected
    std::vector<PersistenceViolation> violations = oracle.check(trace, store);

    assert_no_violations(violations, "MR1.5: Crash before persist (no-op)");
}

// ============================================================================
// Test 6: Oracle Reset Between Traces
// ============================================================================

void test_mr1_oracle_reset() {
    MR1Oracle oracle;

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

        assert_no_violations(violations1, "MR1.6: First trace");
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

        assert_no_violations(violations2, "MR1.6: Second trace (oracle reset)");
    }

    std::cout << "PASS: MR1.6: Oracle reset between traces" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4g.2: MR1 Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_mr1_persist_crash_restart();
    test_mr1_multiple_crashes();
    test_mr1_persist_twice_latest_wins();
    test_mr1_restart_without_persist();
    test_mr1_crash_before_persist();
    test_mr1_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MR1 tests passed ===" << std::endl;
    std::cout << "MR1: State Survives Restart Correctly ✅" << std::endl;

    return 0;
}
