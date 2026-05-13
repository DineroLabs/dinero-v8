/**
 * Ring 7 Phase 7b: Semantic Safety Property Tests (S1-S5)
 *
 * Tests the semantic safety oracles that verify execution has exactly one meaning.
 *
 * Properties tested:
 * - S1: Script Determinism - Same inputs → same result
 * - S2: No Alternate Witness Equivalence - Different witnesses → different execution paths
 * - S3: Taproot Leaf Isolation - Revealing one leaf doesn't enable others
 * - S4: Key-Path ≠ Script-Path Semantics - Distinct execution paths have distinct meanings
 * - S5: Script Version Strictness - No version downgrade ambiguity
 */

#include <gtest/gtest.h>
#include "../properties/semantic_safety_oracle_s1.h"
#include "../properties/semantic_safety_oracle_s2.h"
#include "../properties/semantic_safety_oracle_s3.h"
#include "../properties/semantic_safety_oracle_s4.h"
#include "../properties/semantic_safety_oracle_s5.h"
#include "../framework/execution_simulator.h"

using namespace dinero::execution::test;

class SemanticSafetyTest : public ::testing::Test {
protected:
    void SetUp() override {
        rng = std::make_unique<dinero::p2p::test::PropertyTestRNG>(42);
        simulator = std::make_unique<ExecutionSimulator>(42);
    }

    std::unique_ptr<dinero::p2p::test::PropertyTestRNG> rng;
    std::unique_ptr<ExecutionSimulator> simulator;
};

// ============================================================================
// S1: Script Determinism Tests
// ============================================================================

TEST_F(SemanticSafetyTest, S1_NoViolation_WellFormedTrace) {
    // Simple valid script execution
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s1_valid");

    S1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Well-formed trace should not violate S1";
}

TEST_F(SemanticSafetyTest, S1_NoViolation_ComplexScript) {
    // More complex script: OP_2 OP_3 OP_ADD
    std::vector<uint8_t> script = {0x52, 0x53, 0x93};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s1_complex");

    S1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Complex script should not violate S1";
    EXPECT_TRUE(trace.wasSuccessful());
}

TEST_F(SemanticSafetyTest, S1_Violation_MalformedTrace) {
    // Create malformed trace manually
    ExecutionTrace trace(42, "s1_malformed");
    trace.script = {0x51};
    trace.success = true;

    // Manually create operation
    Operation op(OperationType::OP_PUSH, 0, true);
    trace.operations.push_back(op);

    // Set wrong operation count
    trace.operation_count = 999;  // Mismatch!

    trace.final_hash = trace.computeHash();

    S1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect operation count mismatch";
    EXPECT_EQ(violations[0].property_name, "S1: Script Determinism");
}

TEST_F(SemanticSafetyTest, S1_Violation_HashMismatch) {
    // Create trace with wrong hash
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s1_hash");

    // Corrupt the hash
    trace.final_hash = 0xDEADBEEF;

    S1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect hash mismatch";
}

// ============================================================================
// S2: No Alternate Witness Equivalence Tests
// ============================================================================

TEST_F(SemanticSafetyTest, S2_NoViolation_WitnessUsed) {
    // Script with witness that uses stack operations
    std::vector<uint8_t> script = {0x93};  // OP_ADD (will add witness elements)
    WitnessStack witness;
    witness.push({0x02});  // Push 2
    witness.push({0x03});  // Push 3

    auto trace = simulator->executeScript(script, witness, "s2_witness_used");

    S2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Properly used witness should not violate S2";
}

TEST_F(SemanticSafetyTest, S2_NoViolation_EmptyWitness) {
    // No witness - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;  // Empty

    auto trace = simulator->executeScript(script, witness, "s2_no_witness");

    S2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Empty witness should not violate S2";
}

TEST_F(SemanticSafetyTest, S2_Violation_WitnessNotPushed) {
    // Create trace where witness exists but not pushed
    ExecutionTrace trace(42, "s2_not_pushed");
    trace.script = {0x51};
    trace.witness.push({0xAA});  // Witness provided
    trace.witness.push({0xBB});

    // But no operations recorded (witness not pushed!)
    trace.operation_count = 0;
    trace.success = true;
    trace.final_hash = trace.computeHash();

    S2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect witness not pushed";
    EXPECT_EQ(violations[0].property_name, "S2: No Alternate Witness Equivalence");
}

// ============================================================================
// S3: Taproot Leaf Isolation Tests
// ============================================================================

TEST_F(SemanticSafetyTest, S3_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s3_no_taproot");

    S3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S3";
}

TEST_F(SemanticSafetyTest, S3_NoViolation_KeyPath) {
    // Key path execution
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s3_key_path");

    S3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S3";
}

TEST_F(SemanticSafetyTest, S3_Violation_KeyPathWithReveals) {
    // Create invalid trace: key path with path reveals
    ExecutionTrace trace(42, "s3_invalid");
    trace.script = {0x51};
    trace.taproot_path = TaprootPath::keyPath({0x02, 0x03});

    // Add path reveal (invalid for key path!)
    PathActivation reveal;
    reveal.step = 0;
    reveal.leaf_index = 0;
    trace.path_reveals.push_back(reveal);

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect key path with reveals";
    EXPECT_EQ(violations[0].property_name, "S3: Taproot Leaf Isolation");
}

// ============================================================================
// S4: Key-Path ≠ Script-Path Semantics Tests
// ============================================================================

TEST_F(SemanticSafetyTest, S4_NoViolation_NoTaproot) {
    // No Taproot - property trivially holds
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s4_no_taproot");

    S4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Taproot should not violate S4";
}

TEST_F(SemanticSafetyTest, S4_NoViolation_KeyPath) {
    // Key path execution
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness;
    TaprootPath path = TaprootPath::keyPath({0x02, 0x03});

    auto trace = simulator->executeWithTaproot(script, witness, path, "s4_key_path");

    S4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Key path should not violate S4";
}

TEST_F(SemanticSafetyTest, S4_Violation_MixedPathOps) {
    // Create invalid trace: both key path and script path operations
    ExecutionTrace trace(42, "s4_mixed");
    trace.script = {0x51};
    trace.taproot_path = TaprootPath::keyPath({0x02, 0x03});

    // Add key path operation
    Operation key_op(OperationType::KEY_PATH_EXECUTE, 0, true);
    trace.operations.push_back(key_op);

    // Add script path operation (invalid!)
    Operation script_op(OperationType::LEAF_REVEAL, 1, true);
    trace.operations.push_back(script_op);

    trace.success = true;
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect mixed path operations";
    EXPECT_EQ(violations[0].property_name, "S4: Key-Path ≠ Script-Path Semantics");
}

// ============================================================================
// S5: Script Version Strictness Tests
// ============================================================================

TEST_F(SemanticSafetyTest, S5_NoViolation_ValidScript) {
    // Valid basic script (version 0)
    std::vector<uint8_t> script = {0x51, 0x52, 0x93};
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s5_valid");

    S5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Valid script should not violate S5";
}

TEST_F(SemanticSafetyTest, S5_Violation_EmptyScript) {
    // Create trace with empty script
    ExecutionTrace trace(42, "s5_empty");
    trace.script = {};  // Empty!
    trace.success = false;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect empty script";
    EXPECT_EQ(violations[0].property_name, "S5: Script Version Strictness");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
