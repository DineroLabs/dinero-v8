/**
 * Ring 4 Phase 4b: Framework Self-Tests
 *
 * Purpose: Verify framework infrastructure (NOT mining correctness)
 * Tests: Determinism, crash/restart semantics, trace replay
 */

#include "mining_simulator.h"
#include "mining_sequence_generator.h"
#include "crash_injection_model.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace mining_test;

// ============================================================================
// Test Utilities
// ============================================================================

void assert_true(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

void assert_eq(uint64_t a, uint64_t b, const std::string& msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

void assert_eq(uint32_t a, uint32_t b, const std::string& msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

// Note: size_t overload removed - on 64-bit Linux, size_t == uint64_t
// Implicit conversion to uint64_t handles size_t arguments

// ============================================================================
// Test 1: Deterministic RNG
// ============================================================================

void test_deterministic_rng() {
    std::cout << "Running test_deterministic_rng..." << std::endl;

    const uint64_t seed = 12345;

    // Create two RNGs with same seed
    DeterministicRNG rng1(seed);
    DeterministicRNG rng2(seed);

    // They should produce identical sequences
    for (int i = 0; i < 100; i++) {
        assert_eq(rng1.next(), rng2.next(), "RNG should produce same sequence with same seed");
    }

    // After reset, should reproduce same sequence
    rng1.reset();
    rng2.reset();

    for (int i = 0; i < 100; i++) {
        assert_eq(rng1.next(), rng2.next(), "RNG should reproduce same sequence after reset");
    }

    std::cout << "  ✅ Deterministic RNG test passed" << std::endl;
}

// ============================================================================
// Test 2: Deterministic Clock
// ============================================================================

void test_deterministic_clock() {
    std::cout << "Running test_deterministic_clock..." << std::endl;

    DeterministicClock clock;

    assert_eq(clock.now(), uint64_t(0), "Clock should start at 0");

    clock.advance(10);
    assert_eq(clock.now(), uint64_t(10), "Clock should advance by delta");

    clock.advance(5);
    assert_eq(clock.now(), uint64_t(15), "Clock should accumulate advances");

    clock.setTime(100);
    assert_eq(clock.now(), uint64_t(100), "Clock should set to specific time");

    clock.reset();
    assert_eq(clock.now(), uint64_t(0), "Clock should reset to 0");

    std::cout << "  ✅ Deterministic clock test passed" << std::endl;
}

// ============================================================================
// Test 3: Simulator Determinism - Same Seed Same Trace
// ============================================================================

void test_simulator_determinism() {
    std::cout << "Running test_simulator_determinism..." << std::endl;

    const uint64_t seed = 42;

    // Run simulator twice with same seed and actions
    MiningSimulator sim1(seed);
    MiningSimulator sim2(seed);

    // Apply identical action sequences
    for (int i = 0; i < 50; i++) {
        MiningAction action;
        action.type = (i == 0) ? MiningActionType::START_MINING : MiningActionType::TIME_ADVANCED;
        action.timestamp = i;

        sim1.applyAction(action);
        sim2.applyAction(action);
    }

    // Extract traces
    MiningTrace trace1 = sim1.extractTrace();
    MiningTrace trace2 = sim2.extractTrace();

    // Traces should be identical
    assert_true(trace1 == trace2, "Traces should be identical with same seed");
    assert_true(trace1.hasSameHashAs(trace2), "Trace hashes should match");
    assert_eq(trace1.final_hash, trace2.final_hash, "Final hashes should match");

    std::cout << "  ✅ Simulator determinism test passed" << std::endl;
}

// ============================================================================
// Test 4: Generator Determinism - Same Seed Same Sequence
// ============================================================================

void test_generator_determinism() {
    std::cout << "Running test_generator_determinism..." << std::endl;

    const uint64_t seed = 99;

    // Create two generators with same seed
    MiningSequenceGenerator gen1(seed);
    MiningSequenceGenerator gen2(seed);

    // Generate random scenarios
    auto actions1 = gen1.generateRandomScenario(100);
    auto actions2 = gen2.generateRandomScenario(100);

    // Action sequences should be identical
    assert_eq(static_cast<uint64_t>(actions1.size()), static_cast<uint64_t>(actions2.size()), "Generated action sequences should have same size");

    for (size_t i = 0; i < actions1.size(); i++) {
        assert_true(actions1[i] == actions2[i], "Generated actions should be identical");
    }

    std::cout << "  ✅ Generator determinism test passed" << std::endl;
}

// ============================================================================
// Test 5: Crash Clears Volatile State
// ============================================================================

void test_crash_clears_volatile_state() {
    std::cout << "Running test_crash_clears_volatile_state..." << std::endl;

    MiningSimulator sim(123);

    // Start mining (creates template)
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    sim.applyAction(start);

    MiningState before_crash = sim.getCurrentState();

    // Verify template exists
    assert_true(before_crash.template_prev_hash.has_value(), "Template should exist before crash");
    assert_true(before_crash.template_height.has_value(), "Template height should exist before crash");
    assert_true(before_crash.phase == MiningPhase::MINING, "Should be mining before crash");

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 1;
    sim.applyAction(crash);

    MiningState after_crash = sim.getCurrentState();

    // Volatile state should be cleared
    assert_true(!after_crash.template_prev_hash.has_value(), "Template should be cleared after crash");
    assert_true(!after_crash.template_height.has_value(), "Template height should be cleared after crash");
    assert_true(!after_crash.template_subsidy.has_value(), "Template subsidy should be cleared after crash");
    assert_true(!after_crash.template_tx_count.has_value(), "Template tx count should be cleared after crash");
    assert_true(after_crash.phase == MiningPhase::STOPPED, "Phase should be STOPPED after crash");
    assert_true(after_crash.has_crashed, "Crashed flag should be set");

    // Verify using crash validator
    assert_true(RestartValidator::validateCrash(before_crash, after_crash), "Crash validation should pass");

    std::cout << "  ✅ Crash clears volatile state test passed" << std::endl;
}

// ============================================================================
// Test 6: Restart Preserves Durable State
// ============================================================================

void test_restart_preserves_durable_state() {
    std::cout << "Running test_restart_preserves_durable_state..." << std::endl;

    MiningSimulator sim(456);

    // Add some transactions
    for (int i = 0; i < 10; i++) {
        MiningAction tx_add;
        tx_add.type = MiningActionType::TX_ADDED_TO_MEMPOOL;
        tx_add.timestamp = i;
        tx_add.tx_hash = i + 1000;
        sim.applyAction(tx_add);
    }

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 10;
    sim.applyAction(start);

    MiningState before_crash = sim.getCurrentState();

    // Crash
    MiningAction crash;
    crash.type = MiningActionType::CRASH;
    crash.timestamp = 11;
    sim.applyAction(crash);

    // Restart
    MiningAction restart;
    restart.type = MiningActionType::RESTART;
    restart.timestamp = 12;
    sim.applyAction(restart);

    MiningState after_restart = sim.getCurrentState();

    // Durable state should be preserved
    assert_eq(before_crash.current_tip, after_restart.current_tip, "Chain tip should be preserved");
    assert_eq(before_crash.current_height, after_restart.current_height, "Chain height should be preserved");
    assert_eq(before_crash.mempool_size, after_restart.mempool_size, "Mempool size should be preserved");
    assert_eq(before_crash.mempool_total_fees, after_restart.mempool_total_fees, "Mempool fees should be preserved");
    assert_eq(before_crash.blocks_found, after_restart.blocks_found, "Blocks found should be preserved");

    // Volatile state should be cleared
    assert_true(!after_restart.template_prev_hash.has_value(), "Template should be cleared after restart");
    assert_true(!after_restart.has_crashed, "Crashed flag should be cleared after restart");

    // Restart count should increment
    assert_eq(after_restart.restart_count, before_crash.restart_count + 1, "Restart count should increment");

    // Verify using restart validator
    assert_true(RestartValidator::validateRestart(before_crash, after_restart), "Restart validation should pass");

    std::cout << "  ✅ Restart preserves durable state test passed" << std::endl;
}

// ============================================================================
// Test 7: Multiple Restarts
// ============================================================================

void test_multiple_restarts() {
    std::cout << "Running test_multiple_restarts..." << std::endl;

    MiningSimulator sim(789);

    uint32_t expected_restart_count = 0;

    for (int cycle = 0; cycle < 3; cycle++) {
        // Start mining
        MiningAction start;
        start.type = MiningActionType::START_MINING;
        start.timestamp = cycle * 10;
        sim.applyAction(start);

        // Mine for a bit
        for (int i = 0; i < 5; i++) {
            MiningAction time_adv;
            time_adv.type = MiningActionType::TIME_ADVANCED;
            time_adv.timestamp = cycle * 10 + i + 1;
            sim.applyAction(time_adv);
        }

        // Crash
        MiningAction crash;
        crash.type = MiningActionType::CRASH;
        crash.timestamp = cycle * 10 + 6;
        sim.applyAction(crash);

        // Restart
        MiningAction restart;
        restart.type = MiningActionType::RESTART;
        restart.timestamp = cycle * 10 + 7;
        sim.applyAction(restart);

        expected_restart_count++;

        MiningState state = sim.getCurrentState();
        assert_eq(state.restart_count, expected_restart_count, "Restart count should match expected");
        assert_true(!state.has_crashed, "Should not be crashed after restart");
    }

    std::cout << "  ✅ Multiple restarts test passed" << std::endl;
}

// ============================================================================
// Test 8: Trace Hash Determinism
// ============================================================================

void test_trace_hash_determinism() {
    std::cout << "Running test_trace_hash_determinism..." << std::endl;

    const uint64_t seed = 555;

    // Run same scenario multiple times
    std::vector<uint64_t> hashes;

    for (int run = 0; run < 5; run++) {
        MiningSimulator sim(seed);
        MiningSequenceGenerator gen(seed);

        auto actions = gen.generateSimpleScenario();

        for (const auto& action : actions) {
            sim.applyAction(action);
        }

        MiningTrace trace = sim.extractTrace();
        hashes.push_back(trace.final_hash);
    }

    // All hashes should be identical
    for (size_t i = 1; i < hashes.size(); i++) {
        assert_eq(hashes[0], hashes[i], "Trace hashes should be identical across runs");
    }

    std::cout << "  ✅ Trace hash determinism test passed" << std::endl;
}

// ============================================================================
// Test 9: Predefined Scenarios Execute Without Errors
// ============================================================================

void test_predefined_scenarios() {
    std::cout << "Running test_predefined_scenarios..." << std::endl;

    const uint64_t seed = 111;

    MiningSequenceGenerator gen(seed);

    // Test all predefined scenarios
    std::vector<std::pair<std::string, std::vector<MiningAction>>> scenarios = {
        {"Simple", gen.generateSimpleScenario()},
        {"Restart", gen.generateRestartScenario()},
        {"Reorg", gen.generateReorgScenario()},
        {"Crash", gen.generateCrashScenario()},
        {"Mempool", gen.generateMempoolChurnScenario()}
    };

    for (const auto& [name, actions] : scenarios) {
        MiningSimulator sim(seed);

        // Apply all actions (should not crash/throw)
        for (const auto& action : actions) {
            sim.applyAction(action);
        }

        // Should have events recorded
        assert_true(!sim.getAllEvents().empty(), name + " scenario should record events");

        // Should be able to extract trace
        MiningTrace trace = sim.extractTrace();
        assert_true(!trace.actions.empty(), name + " trace should have actions");
        assert_true(!trace.events.empty(), name + " trace should have events");
    }

    std::cout << "  ✅ Predefined scenarios test passed" << std::endl;
}

// ============================================================================
// Test 10: Crash Injection Determinism
// ============================================================================

void test_crash_injection_determinism() {
    std::cout << "Running test_crash_injection_determinism..." << std::endl;

    const uint64_t seed = 333;

    CrashPolicy policy = CrashPolicy::frequentCrashes(0.1, 3);

    // Generate base scenario
    MiningSequenceGenerator gen(seed);
    auto base_actions = gen.generateRandomScenario(100);

    // Inject crashes twice with same seed
    CrashInjector injector1(policy, seed);
    CrashInjector injector2(policy, seed);

    auto injected1 = injector1.injectCrashes(base_actions);
    auto injected2 = injector2.injectCrashes(base_actions);

    // Should produce identical results
    assert_eq(static_cast<uint64_t>(injected1.size()), static_cast<uint64_t>(injected2.size()), "Injected sequences should have same size");

    for (size_t i = 0; i < injected1.size(); i++) {
        assert_true(injected1[i] == injected2[i], "Injected actions should be identical");
    }

    std::cout << "  ✅ Crash injection determinism test passed" << std::endl;
}

// ============================================================================
// Test 11: State Classification
// ============================================================================

void test_state_classification() {
    std::cout << "Running test_state_classification..." << std::endl;

    MiningState state;
    state.current_tip = 0xABCD;
    state.current_height = 100;
    state.mempool_size = 50;
    state.template_prev_hash = 0x1234;
    state.template_height = 101;

    // Extract durable state
    auto durable = StateClassification::extractDurable(state);
    assert_eq(durable.current_tip, uint64_t(0xABCD), "Durable state should preserve chain tip");
    assert_eq(durable.current_height, uint32_t(100), "Durable state should preserve height");
    assert_eq(durable.mempool_size, uint32_t(50), "Durable state should preserve mempool size");

    // Extract volatile state
    auto volatile_state = StateClassification::extractVolatile(state);
    assert_true(volatile_state.template_prev_hash.has_value(), "Volatile state should include template");
    assert_eq(*volatile_state.template_prev_hash, uint64_t(0x1234), "Template hash should match");
    assert_true(volatile_state.template_height.has_value(), "Volatile state should include template height");
    assert_eq(*volatile_state.template_height, uint32_t(101), "Template height should match");

    std::cout << "  ✅ State classification test passed" << std::endl;
}

// ============================================================================
// Test 12: Checkpoint and Restore
// ============================================================================

void test_checkpoint_restore() {
    std::cout << "Running test_checkpoint_restore..." << std::endl;

    MiningSimulator sim(999);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    sim.applyAction(start);

    // Save checkpoint
    sim.saveCheckpoint();
    MiningState checkpoint_state = sim.getCurrentState();

    // Continue and modify state
    for (int i = 0; i < 10; i++) {
        MiningAction time_adv;
        time_adv.type = MiningActionType::TIME_ADVANCED;
        time_adv.timestamp = i + 1;
        sim.applyAction(time_adv);
    }

    MiningState modified_state = sim.getCurrentState();
    assert_true(!(checkpoint_state == modified_state), "Modified state should differ from checkpoint");

    // Restore checkpoint
    sim.restoreLastCheckpoint();
    MiningState restored_state = sim.getCurrentState();

    // Should match checkpoint state
    assert_true(checkpoint_state == restored_state, "Restored state should match checkpoint");

    std::cout << "  ✅ Checkpoint and restore test passed" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "Ring 4 Phase 4b: Mining Framework Self-Tests" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

    try {
        test_deterministic_rng();
        test_deterministic_clock();
        test_simulator_determinism();
        test_generator_determinism();
        test_crash_clears_volatile_state();
        test_restart_preserves_durable_state();
        test_multiple_restarts();
        test_trace_hash_determinism();
        test_predefined_scenarios();
        test_crash_injection_determinism();
        test_state_classification();
        test_checkpoint_restore();

        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "✅ ALL TESTS PASSED (12/12)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
