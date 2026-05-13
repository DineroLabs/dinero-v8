/**
 * Ring 4 Phase 4c: Mining Correctness Oracle Tests
 *
 * Purpose: Verify oracle correctly validates properties against traces
 * Tests: MC1-MC5 property checking with Phase 4b framework
 */

#include "mining_correctness_oracle.h"
#include "../framework/mining_simulator.h"
#include "../framework/mining_sequence_generator.h"
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

void assert_eq(size_t a, size_t b, const std::string& msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// Test 1: Oracle Construction
// ============================================================================

void test_oracle_construction() {
    std::cout << "Running test_oracle_construction..." << std::endl;

    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    assert_true(oracle.getParams() == params, "Oracle should preserve params");
    assert_eq(oracle.getSubsidyCalculator().getHalvingInterval(), uint32_t(150),
              "Regtest halving interval should be 150");

    std::cout << "  ✅ Oracle construction test passed" << std::endl;
}

// ============================================================================
// Test 2: MC1 Subsidy Correctness - Valid Trace
// ============================================================================

void test_mc1_valid_subsidy() {
    std::cout << "Running test_mc1_valid_subsidy..." << std::endl;

    // Create simulator with correct subsidy (100 DIN placeholder)
    const uint64_t seed = 42;
    MiningSimulator sim(seed);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    sim.applyAction(start);

    // Extract trace
    MiningTrace trace = sim.extractTrace();

    // Check with oracle
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    auto violations = oracle.checkSubsidyCorrectness(trace);

    // Should have no violations (simulator uses correct 100 DIN)
    assert_eq(violations.size(), size_t(0), "Should have no subsidy violations");

    std::cout << "  ✅ MC1 valid subsidy test passed" << std::endl;
}

// ============================================================================
// Test 3: MC2 Coinbase Structure - Metadata Present
// ============================================================================

void test_mc2_coinbase_structure() {
    std::cout << "Running test_mc2_coinbase_structure..." << std::endl;

    const uint64_t seed = 123;
    MiningSimulator sim(seed);

    // Start mining (creates template with metadata)
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    sim.applyAction(start);

    // Extract trace
    MiningTrace trace = sim.extractTrace();

    // Check with oracle
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    auto violations = oracle.checkCoinbaseStructure(trace);

    // Should have no violations (metadata is present)
    assert_eq(violations.size(), size_t(0), "Should have no coinbase violations");

    std::cout << "  ✅ MC2 coinbase structure test passed" << std::endl;
}

// ============================================================================
// Test 4: MC3 Template Validity - Consistent Heights
// ============================================================================

void test_mc3_template_validity() {
    std::cout << "Running test_mc3_template_validity..." << std::endl;

    const uint64_t seed = 456;
    MiningSimulator sim(seed);

    // Generate simple scenario
    MiningSequenceGenerator gen(seed);
    auto actions = gen.generateSimpleScenario();

    for (const auto& action : actions) {
        sim.applyAction(action);
    }

    // Extract trace
    MiningTrace trace = sim.extractTrace();

    // Check with oracle
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    auto violations = oracle.checkTemplateValidity(trace);

    // Should have no violations (heights are consistent)
    assert_eq(violations.size(), size_t(0), "Should have no template violations");

    std::cout << "  ✅ MC3 template validity test passed" << std::endl;
}

// ============================================================================
// Test 5: MC4 Transaction Context - Valid Counts
// ============================================================================

void test_mc4_transaction_context() {
    std::cout << "Running test_mc4_transaction_context..." << std::endl;

    const uint64_t seed = 789;
    MiningSimulator sim(seed);

    // Start mining
    MiningAction start;
    start.type = MiningActionType::START_MINING;
    start.timestamp = 0;
    sim.applyAction(start);

    // Add some transactions
    for (int i = 0; i < 10; i++) {
        MiningAction tx_add;
        tx_add.type = MiningActionType::TX_ADDED_TO_MEMPOOL;
        tx_add.timestamp = i + 1;
        tx_add.tx_hash = i + 1000;
        sim.applyAction(tx_add);
    }

    // Extract trace
    MiningTrace trace = sim.extractTrace();

    // Check with oracle
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    auto violations = oracle.checkTransactionContext(trace);

    // Should have no violations (tx count is reasonable)
    assert_eq(violations.size(), size_t(0), "Should have no context violations");

    std::cout << "  ✅ MC4 transaction context test passed" << std::endl;
}

// ============================================================================
// Test 6: MC5 No Consensus Bypass - Crash/Restart Handling
// ============================================================================

void test_mc5_no_consensus_bypass() {
    std::cout << "Running test_mc5_no_consensus_bypass..." << std::endl;

    const uint64_t seed = 999;
    MiningSimulator sim(seed);

    // Generate crash scenario
    MiningSequenceGenerator gen(seed);
    auto actions = gen.generateCrashScenario();

    for (const auto& action : actions) {
        sim.applyAction(action);
    }

    // Extract trace
    MiningTrace trace = sim.extractTrace();

    // Check with oracle
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    auto violations = oracle.checkNoConsensusBypass(trace);

    // Should have no violations (crash/restart handled correctly)
    assert_eq(violations.size(), size_t(0), "Should have no bypass violations");

    std::cout << "  ✅ MC5 no consensus bypass test passed" << std::endl;
}

// ============================================================================
// Test 7: Check All Properties - Clean Trace
// ============================================================================

void test_check_all_properties() {
    std::cout << "Running test_check_all_properties..." << std::endl;

    const uint64_t seed = 111;
    MiningSimulator sim(seed);

    // Generate mempool churn scenario (complex but valid)
    MiningSequenceGenerator gen(seed);
    auto actions = gen.generateMempoolChurnScenario();

    for (const auto& action : actions) {
        sim.applyAction(action);
    }

    // Extract trace
    MiningTrace trace = sim.extractTrace();

    // Check all properties
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    CorrectnessReport report = oracle.checkAllProperties(trace);

    // Should have no violations
    assert_true(report.allPropertiesSatisfied(), "All properties should be satisfied");
    assert_eq(report.totalViolations(), size_t(0), "Should have zero total violations");

    std::cout << "  ✅ Check all properties test passed" << std::endl;
}

// ============================================================================
// Test 8: Multiple Scenarios - No Violations
// ============================================================================

void test_multiple_scenarios() {
    std::cout << "Running test_multiple_scenarios..." << std::endl;

    const uint64_t seed = 222;
    ConsensusParams params = ConsensusParams::regtest();
    MiningCorrectnessOracle oracle(params);

    // Test all predefined scenarios
    MiningSequenceGenerator gen(seed);

    std::vector<std::pair<std::string, std::vector<MiningAction>>> scenarios = {
        {"Simple", gen.generateSimpleScenario()},
        {"Restart", gen.generateRestartScenario()},
        {"Reorg", gen.generateReorgScenario()},
        {"Crash", gen.generateCrashScenario()},
        {"Mempool", gen.generateMempoolChurnScenario()}
    };

    for (const auto& [name, actions] : scenarios) {
        MiningSimulator sim(seed);

        for (const auto& action : actions) {
            sim.applyAction(action);
        }

        MiningTrace trace = sim.extractTrace();
        CorrectnessReport report = oracle.checkAllProperties(trace);

        assert_true(report.allPropertiesSatisfied(),
                    name + " scenario should satisfy all properties");
    }

    std::cout << "  ✅ Multiple scenarios test passed" << std::endl;
}

// ============================================================================
// Test 9: Correctness Report Structure
// ============================================================================

void test_correctness_report() {
    std::cout << "Running test_correctness_report..." << std::endl;

    // Create empty report
    CorrectnessReport report;

    assert_true(report.allPropertiesSatisfied(), "Empty report should be satisfied");
    assert_eq(report.totalViolations(), size_t(0), "Empty report should have 0 violations");

    // Add a violation
    report.subsidy_violations.emplace_back(100, 1000, 2000, "Test violation");

    assert_true(!report.allPropertiesSatisfied(), "Report with violations should not be satisfied");
    assert_eq(report.totalViolations(), size_t(1), "Report should have 1 violation");

    std::cout << "  ✅ Correctness report test passed" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "Ring 4 Phase 4c: Mining Correctness Oracle Tests" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

    try {
        test_oracle_construction();
        test_mc1_valid_subsidy();
        test_mc2_coinbase_structure();
        test_mc3_template_validity();
        test_mc4_transaction_context();
        test_mc5_no_consensus_bypass();
        test_check_all_properties();
        test_multiple_scenarios();
        test_correctness_report();

        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "✅ ALL TESTS PASSED (9/9)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
