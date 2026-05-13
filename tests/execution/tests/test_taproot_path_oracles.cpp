/**
 * Ring 7 Phase 7c: Taproot Path Safety Property Tests (S6-S10)
 *
 * Tests the Taproot path safety oracles that verify hidden paths are safe.
 *
 * Properties tested:
 * - S6: Hidden Path Non-Activation - Unrevealed paths cannot execute
 * - S7: Partial Reveal Safety - Revealing subset of paths is safe
 * - S8: No Semantic Leakage - Unused leaves don't affect active execution
 * - S9: Path Commitment Completeness - All executable paths are committed
 * - S10: Leaf Execution Uniqueness - Each leaf executes exactly once per input
 */

#include <gtest/gtest.h>
#include "../properties/taproot_path_oracle_s6.h"
#include "../properties/taproot_path_oracle_s7.h"
#include "../properties/taproot_path_oracle_s8.h"
#include "../properties/taproot_path_oracle_s9.h"
#include "../properties/taproot_path_oracle_s10.h"
#include "../framework/execution_simulator.h"

using namespace dinero::execution::test;

class TaprootPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        rng = std::make_unique<dinero::p2p::test::PropertyTestRNG>(42);
        simulator = std::make_unique<ExecutionSimulator>(42);
    }

    std::unique_ptr<dinero::p2p::test::PropertyTestRNG> rng;
    std::unique_ptr<ExecutionSimulator> simulator;
};

// ============================================================================
// S6: Hidden Path Non-Activation Tests
// ============================================================================

TEST_F(TaprootPathTest, S6_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s6_no_taproot");

    S6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S6";
}

TEST_F(TaprootPathTest, S6_NoViolation_KeyPath) {
    // Key path - no reveals expected
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s6_key_path");

    S6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S6";
}

TEST_F(TaprootPathTest, S6_Violation_KeyPathWithReveals) {
    // Create invalid trace: key path with reveals
    ExecutionTrace trace(42, "s6_invalid_key");
    trace.script = {0x51};
    trace.taproot_path = TaprootPath::keyPath({0x02, 0x03});

    // Add invalid path reveal
    PathActivation reveal;
    reveal.step = 0;
    reveal.leaf_index = 0;
    trace.path_reveals.push_back(reveal);

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect key path with reveals";
    EXPECT_EQ(violations[0].property_name, "S6: Hidden Path Non-Activation");
}

TEST_F(TaprootPathTest, S6_Violation_ScriptPathNoReveals) {
    // Create invalid trace: script path without reveals
    ExecutionTrace trace(42, "s6_script_no_reveals");
    trace.script = {0x51};
    TaprootPath path;
    path.is_key_path = false;
    path.internal_key = {0x02, 0x03};
    trace.taproot_path = path;

    // No path reveals (invalid!)
    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect script path without reveals";
}

// ============================================================================
// S7: Partial Reveal Safety Tests
// ============================================================================

TEST_F(TaprootPathTest, S7_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s7_no_taproot");

    S7Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S7";
}

TEST_F(TaprootPathTest, S7_NoViolation_KeyPath) {
    // Key path - no partial reveals possible
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s7_key_path");

    S7Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S7";
}

TEST_F(TaprootPathTest, S7_Violation_DuplicateLeafReveal) {
    // Create invalid trace: same leaf revealed twice
    ExecutionTrace trace(42, "s7_duplicate");
    trace.script = {0x51};
    TaprootPath path;
    path.is_key_path = false;
    trace.taproot_path = path;

    // Add duplicate reveals
    PathActivation reveal1;
    reveal1.step = 0;
    reveal1.leaf_index = 0;
    trace.path_reveals.push_back(reveal1);

    PathActivation reveal2;
    reveal2.step = 1;
    reveal2.leaf_index = 0;  // Same leaf!
    trace.path_reveals.push_back(reveal2);

    trace.success = true;
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S7Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect duplicate leaf reveal";
    EXPECT_EQ(violations[0].property_name, "S7: Partial Reveal Safety");
}

// ============================================================================
// S8: No Semantic Leakage Tests
// ============================================================================

TEST_F(TaprootPathTest, S8_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s8_no_taproot");

    S8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S8";
}

TEST_F(TaprootPathTest, S8_NoViolation_KeyPath) {
    // Key path - no script path operations
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s8_key_path");

    S8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S8";
}

TEST_F(TaprootPathTest, S8_Violation_KeyPathWithScriptOps) {
    // Create invalid trace: key path with script operations
    ExecutionTrace trace(42, "s8_leakage");
    trace.script = {0x51};
    trace.taproot_path = TaprootPath::keyPath({0x02, 0x03});

    // Add script path operation (leakage!)
    Operation op(OperationType::LEAF_REVEAL, 0, true);
    trace.operations.push_back(op);

    trace.success = true;
    trace.operation_count = 1;
    trace.final_hash = trace.computeHash();

    S8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect script operation in key path";
    EXPECT_EQ(violations[0].property_name, "S8: No Semantic Leakage from Unused Leaves");
}

// ============================================================================
// S9: Path Commitment Completeness Tests
// ============================================================================

TEST_F(TaprootPathTest, S9_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s9_no_taproot");

    S9Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S9";
}

TEST_F(TaprootPathTest, S9_NoViolation_KeyPath) {
    // Key path - no commitment needed
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s9_key_path");

    S9Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S9";
}

TEST_F(TaprootPathTest, S9_Violation_ScriptPathNoLeaf) {
    // Create invalid trace: script path without revealed leaf
    ExecutionTrace trace(42, "s9_no_leaf");
    trace.script = {0x51};
    TaprootPath path;
    path.is_key_path = false;
    path.internal_key = {0x02, 0x03};
    // No revealed_leaf!
    trace.taproot_path = path;

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S9Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect script path without revealed leaf";
    EXPECT_EQ(violations[0].property_name, "S9: Path Commitment Completeness");
}

// ============================================================================
// S10: Leaf Execution Uniqueness Tests
// ============================================================================

TEST_F(TaprootPathTest, S10_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s10_no_taproot");

    S10Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S10";
}

TEST_F(TaprootPathTest, S10_NoViolation_KeyPath) {
    // Key path - no leaf execution
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s10_key_path");

    S10Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S10";
}

TEST_F(TaprootPathTest, S10_Violation_DuplicateLeafExecution) {
    // Create invalid trace: same leaf executed twice
    ExecutionTrace trace(42, "s10_duplicate");
    trace.script = {0x51};
    TaprootPath path;
    path.is_key_path = false;
    trace.taproot_path = path;

    // Add duplicate leaf reveals
    PathActivation reveal1;
    reveal1.step = 0;
    reveal1.leaf_index = 0;
    trace.path_reveals.push_back(reveal1);

    PathActivation reveal2;
    reveal2.step = 1;
    reveal2.leaf_index = 0;  // Same leaf executed twice!
    trace.path_reveals.push_back(reveal2);

    trace.success = true;
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S10Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect duplicate leaf execution";
    EXPECT_EQ(violations[0].property_name, "S10: Leaf Execution Uniqueness");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
