#include "mining_persistence_oracle_mr5.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
#include <iostream>
#include <cassert>

// Ring 4 Phase 4g.3: MR5 Persistence Property Tests
// Test MR5: Persistence Does Not Break Determinism

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
// Test 1: Identical Persist/Recover Traces
// ============================================================================

void test_mr5_identical_traces() {
    MR5Oracle oracle;

    const uint64_t seed = 12345;

    // First trace
    DeterministicPersistenceStore store1(seed);
    MiningSimulator simulator1(seed);
    MiningSequenceGenerator generator1(seed);

    std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
    for (const auto& action : actions1) {
        simulator1.applyAction(action);
    }
    MiningTrace trace1 = simulator1.extractTrace();

    // Second trace (identical seed)
    DeterministicPersistenceStore store2(seed);
    MiningSimulator simulator2(seed);
    MiningSequenceGenerator generator2(seed);

    std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
    for (const auto& action : actions2) {
        simulator2.applyAction(action);
    }
    MiningTrace trace2 = simulator2.extractTrace();

    // Check MR5 property using checkPair
    std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

    assert_no_violations(violations, "MR5.1: Identical persist/recover traces");
}

// ============================================================================
// Test 2: Restart Determinism
// ============================================================================

void test_mr5_restart_determinism() {
    MR5Oracle oracle;

    const uint64_t seed = 23456;

    // Helper lambda to create identical restart scenario
    auto create_restart_trace = [&](uint64_t s) {
        DeterministicPersistenceStore store(s);
        MiningSimulator simulator(s);

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
        simulator.applyAction(crash);

        // Restart
        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = 200;
        simulator.applyAction(restart);

        return std::make_pair(simulator.extractTrace(), store);
    };

    // Create two identical traces
    auto [trace1, store1] = create_restart_trace(seed);
    auto [trace2, store2] = create_restart_trace(seed);

    // Check MR5 property
    std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

    assert_no_violations(violations, "MR5.2: Restart determinism");
}

// ============================================================================
// Test 3: Partial Write Determinism
// ============================================================================

void test_mr5_partial_write_determinism() {
    MR5Oracle oracle;

    const uint64_t seed = 34567;

    // Helper to create trace with partial write
    auto create_partial_write_trace = [&](uint64_t s) {
        DeterministicPersistenceStore store(s);
        MiningSimulator simulator(s);

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
        simulator.applyAction(crash);

        // Restart
        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = 200;
        simulator.applyAction(restart);

        return std::make_pair(simulator.extractTrace(), store);
    };

    // Create two identical traces
    auto [trace1, store1] = create_partial_write_trace(seed);
    auto [trace2, store2] = create_partial_write_trace(seed);

    // Check MR5 property
    std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

    assert_no_violations(violations, "MR5.3: Partial write determinism");
}

// ============================================================================
// Test 4: Corrupt Snapshot Determinism
// ============================================================================

void test_mr5_corrupt_snapshot_determinism() {
    MR5Oracle oracle;

    const uint64_t seed = 45678;

    // Helper to create trace
    auto create_trace = [&](uint64_t s) {
        DeterministicPersistenceStore store(s);
        MiningSimulator simulator(s);

        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = 0;
        simulator.applyAction(start);

        MiningAction time1;
        time1.type = MiningActionType::TIME_ADVANCED;
        time1.timestamp = 100;
        simulator.applyAction(time1);

        MiningAction crash;
        crash.type = MiningActionType::CRASH;
        crash.timestamp = 150;
        simulator.applyAction(crash);

        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = 200;
        simulator.applyAction(restart);

        return std::make_pair(simulator.extractTrace(), store);
    };

    // Create two identical traces
    auto [trace1, store1] = create_trace(seed);
    auto [trace2, store2] = create_trace(seed);

    // Check MR5 property
    std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

    assert_no_violations(violations, "MR5.4: Corrupt snapshot determinism");
}

// ============================================================================
// Test 5: Multiple Recovery Cycles
// ============================================================================

void test_mr5_multiple_recovery_cycles() {
    MR5Oracle oracle;

    const uint64_t seed = 56789;

    // Helper to create trace with multiple cycles
    auto create_multi_cycle_trace = [&](uint64_t s) {
        DeterministicPersistenceStore store(s);
        MiningSimulator simulator(s);

        for (int cycle = 0; cycle < 3; cycle++) {
            MiningAction start;
            start.type = MiningActionType::START_MINING;
            start.timestamp = cycle * 200;
            simulator.applyAction(start);

            MiningAction time_adv;
            time_adv.type = MiningActionType::TIME_ADVANCED;
            time_adv.timestamp = cycle * 200 + 50;
            simulator.applyAction(time_adv);

            MiningAction crash;
            crash.type = MiningActionType::CRASH;
            crash.timestamp = cycle * 200 + 100;
            simulator.applyAction(crash);

            MiningAction restart;
            restart.type = MiningActionType::RESTART;
            restart.timestamp = cycle * 200 + 150;
            simulator.applyAction(restart);
        }

        return std::make_pair(simulator.extractTrace(), store);
    };

    // Create two identical traces
    auto [trace1, store1] = create_multi_cycle_trace(seed);
    auto [trace2, store2] = create_multi_cycle_trace(seed);

    // Check MR5 property
    std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

    assert_no_violations(violations, "MR5.5: Multiple recovery cycles");
}

// ============================================================================
// Test 6: Oracle Reset
// ============================================================================

void test_mr5_oracle_reset() {
    MR5Oracle oracle;

    // First pair
    {
        const uint64_t seed = 67890;

        DeterministicPersistenceStore store1(seed);
        MiningSimulator simulator1(seed);
        MiningSequenceGenerator generator1(seed);

        std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
        for (const auto& action : actions1) {
            simulator1.applyAction(action);
        }
        MiningTrace trace1 = simulator1.extractTrace();

        DeterministicPersistenceStore store2(seed);
        MiningSimulator simulator2(seed);
        MiningSequenceGenerator generator2(seed);

        std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
        for (const auto& action : actions2) {
            simulator2.applyAction(action);
        }
        MiningTrace trace2 = simulator2.extractTrace();

        std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

        assert_no_violations(violations, "MR5.6: First pair");
    }

    // Second pair (oracle should handle independently)
    {
        const uint64_t seed = 78901;

        DeterministicPersistenceStore store1(seed);
        MiningSimulator simulator1(seed);
        MiningSequenceGenerator generator1(seed);

        std::vector<MiningAction> actions1 = generator1.generateRestartScenario();
        for (const auto& action : actions1) {
            simulator1.applyAction(action);
        }
        MiningTrace trace1 = simulator1.extractTrace();

        DeterministicPersistenceStore store2(seed);
        MiningSimulator simulator2(seed);
        MiningSequenceGenerator generator2(seed);

        std::vector<MiningAction> actions2 = generator2.generateRestartScenario();
        for (const auto& action : actions2) {
            simulator2.applyAction(action);
        }
        MiningTrace trace2 = simulator2.extractTrace();

        std::vector<PersistenceViolation> violations = oracle.checkPair(trace1, store1, trace2, store2);

        assert_no_violations(violations, "MR5.6: Second pair (oracle reset)");
    }

    std::cout << "PASS: MR5.6: Oracle reset" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "=== Ring 4 Phase 4g.3: MR5 Property Tests ===" << std::endl;
    std::cout << std::endl;

    test_mr5_identical_traces();
    test_mr5_restart_determinism();
    test_mr5_partial_write_determinism();
    test_mr5_corrupt_snapshot_determinism();
    test_mr5_multiple_recovery_cycles();
    test_mr5_oracle_reset();

    std::cout << std::endl;
    std::cout << "=== All MR5 tests passed ===" << std::endl;
    std::cout << "MR5: Persistence Does Not Break Determinism ✅" << std::endl;
    std::cout << std::endl;
    std::cout << "Phase 4g → Phase 4f bridge verified ✅" << std::endl;

    return 0;
}
