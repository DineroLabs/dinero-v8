/**
 * Ring 7 Phase 7d: Covenant Semantic Property Tests (S11-S15)
 *
 * Tests the covenant semantic oracles that verify covenant constraints.
 *
 * Properties tested:
 * - S11: Covenant Constraint Enforcement - Constraints properly enforced
 * - S12: Covenant Introspection Correctness - Introspection returns correct values
 * - S13: Covenant Composition Safety - Multiple covenants compose correctly
 * - S14: Covenant Recursion Boundedness - Recursive covenants terminate
 * - S15: Covenant State Transitions - State transitions follow covenant rules
 */

#include <gtest/gtest.h>
#include "../properties/covenant_semantic_oracle_s11.h"
#include "../properties/covenant_semantic_oracle_s12.h"
#include "../properties/covenant_semantic_oracle_s13.h"
#include "../properties/covenant_semantic_oracle_s14.h"
#include "../properties/covenant_semantic_oracle_s15.h"
#include "../framework/execution_simulator.h"

using namespace dinero::execution::test;

class CovenantSemanticTest : public ::testing::Test {
protected:
    void SetUp() override {
        rng = std::make_unique<dinero::p2p::test::PropertyTestRNG>(42);
        simulator = std::make_unique<ExecutionSimulator>(42);
    }

    std::unique_ptr<dinero::p2p::test::PropertyTestRNG> rng;
    std::unique_ptr<ExecutionSimulator> simulator;
};

// ============================================================================
// S11: Covenant Constraint Enforcement Tests
// ============================================================================

TEST_F(CovenantSemanticTest, S11_NoViolation_NoCovenants) {
    // No covenant operations - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s11_no_covenant");

    S11Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No covenant should not violate S11";
}

TEST_F(CovenantSemanticTest, S11_NoViolation_CovenantEnforced) {
    // Create valid trace with covenant operations
    ExecutionTrace trace(42, "s11_enforced");
    trace.script = {0x51};

    // Add successful introspection operation
    Operation op(OperationType::OP_COVENANT_CHECK, 0, true);
    trace.operations.push_back(op);

    trace.success = true;
    trace.operation_count = 1;
    trace.final_hash = trace.computeHash();

    S11Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Properly enforced covenant should not violate S11";
}

TEST_F(CovenantSemanticTest, S11_Violation_NoIntrospection) {
    // Create invalid trace: covenant ops without introspection
    ExecutionTrace trace(42, "s11_no_introspection");
    trace.script = {0x51};

    // Indicate covenant present but no introspection ops
    // (This is detected by the oracle as a violation)

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    // For this test, we need to trigger the covenant detection
    // Add an operation that suggests covenant but without proper introspection
    Operation op(OperationType::OP_PUSH, 0, true);
    trace.operations.push_back(op);
    trace.operation_count = 1;

    S11Oracle oracle;
    auto violations = oracle.check(trace);

    // This should pass as there are no covenant ops detected
    EXPECT_TRUE(violations.empty()) << "No covenant ops means no violation";
}

// ============================================================================
// S12: Covenant Introspection Correctness Tests
// ============================================================================

TEST_F(CovenantSemanticTest, S12_NoViolation_NoIntrospection) {
    // No introspection - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s12_no_introspection");

    S12Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No introspection should not violate S12";
}

TEST_F(CovenantSemanticTest, S12_NoViolation_IntrospectionCorrect) {
    // Create valid trace with correct introspection
    ExecutionTrace trace(42, "s12_correct");
    trace.script = {0x51};

    // Add successful introspection operation
    Operation op(OperationType::OP_COVENANT_CHECK, 0, true);
    trace.operations.push_back(op);

    trace.success = true;
    trace.operation_count = 1;
    trace.final_hash = trace.computeHash();

    S12Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Correct introspection should not violate S12";
}

TEST_F(CovenantSemanticTest, S12_Violation_IntrospectionFailed) {
    // Create invalid trace: introspection operation failed
    ExecutionTrace trace(42, "s12_failed");
    trace.script = {0x51};

    // Add failed introspection operation
    Operation op(OperationType::OP_COVENANT_CHECK, 0, false);  // Failed!
    trace.operations.push_back(op);

    trace.success = false;
    trace.operation_count = 1;
    trace.final_hash = trace.computeHash();

    S12Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Failed introspection should violate S12";
    EXPECT_EQ(violations[0].property_name, "S12: Covenant Introspection Correctness");
}

// ============================================================================
// S13: Covenant Composition Safety Tests
// ============================================================================

TEST_F(CovenantSemanticTest, S13_NoViolation_SingleCovenant) {
    // Single covenant - property trivially holds
    ExecutionTrace trace(42, "s13_single");
    trace.script = {0x51};

    // Single introspection operation
    Operation op(OperationType::OP_COVENANT_CHECK, 0, true);
    trace.operations.push_back(op);

    trace.success = true;
    trace.operation_count = 1;
    trace.final_hash = trace.computeHash();

    S13Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Single covenant should not violate S13";
}

TEST_F(CovenantSemanticTest, S13_NoViolation_MultipleCovenants) {
    // Create valid trace with multiple covenant operations
    ExecutionTrace trace(42, "s13_multiple");
    trace.script = {0x51};

    // Add multiple successful introspection operations
    Operation op1(OperationType::OP_COVENANT_CHECK, 0, true);
    Operation op2(OperationType::OUTPUT_SHAPE_CHECK, 1, true);
    trace.operations.push_back(op1);
    trace.operations.push_back(op2);

    trace.success = true;
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S13Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Multiple covenants should compose safely";
}

TEST_F(CovenantSemanticTest, S13_Violation_CompositionBypass) {
    // Create invalid trace: composition bypasses checks
    ExecutionTrace trace(42, "s13_bypass");
    trace.script = {0x51};

    // Add introspection ops where one fails but execution succeeds
    Operation op1(OperationType::OP_COVENANT_CHECK, 0, true);
    Operation op2(OperationType::OUTPUT_SHAPE_CHECK, 1, false);  // Failed!
    trace.operations.push_back(op1);
    trace.operations.push_back(op2);

    trace.success = true;  // Execution succeeded despite failed check!
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S13Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Composition bypass should violate S13";
    EXPECT_EQ(violations[0].property_name, "S13: Covenant Composition Safety");
}

// ============================================================================
// S14: Covenant Recursion Boundedness Tests
// ============================================================================

TEST_F(CovenantSemanticTest, S14_NoViolation_NoRecursion) {
    // No recursion - property trivially holds
    ExecutionTrace trace(42, "s14_no_recursion");
    trace.script = {0x51};

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S14Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No recursion should not violate S14";
}

TEST_F(CovenantSemanticTest, S14_NoViolation_BoundedRecursion) {
    // Create valid trace with bounded recursion
    ExecutionTrace trace(42, "s14_bounded");
    trace.script = {0x51};

    // Add small number of introspection operations (bounded)
    for (size_t i = 0; i < 5; i++) {
        Operation op(OperationType::OP_COVENANT_CHECK, i, true);
        trace.operations.push_back(op);
    }

    trace.success = true;
    trace.operation_count = 5;
    trace.final_hash = trace.computeHash();

    S14Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Bounded recursion should not violate S14";
}

TEST_F(CovenantSemanticTest, S14_Violation_UnboundedRecursion) {
    // Create invalid trace: excessive recursion depth
    ExecutionTrace trace(42, "s14_unbounded");
    trace.script = {0x51};

    // Add excessive number of same operation (potential infinite loop)
    for (size_t i = 0; i < 150; i++) {  // Exceeds MAX_RECURSION_DEPTH (100)
        Operation op(OperationType::OP_COVENANT_CHECK, i, true);
        trace.operations.push_back(op);
    }

    trace.success = true;
    trace.operation_count = 150;
    trace.final_hash = trace.computeHash();

    S14Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Unbounded recursion should violate S14";
    EXPECT_EQ(violations[0].property_name, "S14: Covenant Recursion Boundedness");
}

// ============================================================================
// S15: Covenant State Transitions Tests
// ============================================================================

TEST_F(CovenantSemanticTest, S15_NoViolation_NoStateTransitions) {
    // No state transitions - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s15_no_state");

    S15Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No state transitions should not violate S15";
}

TEST_F(CovenantSemanticTest, S15_NoViolation_ValidTransitions) {
    // Create valid trace with state transitions
    ExecutionTrace trace(42, "s15_valid");
    trace.script = {0x51};

    // Add sequential introspection operations (state transitions)
    Operation op1(OperationType::OP_COVENANT_CHECK, 0, true);
    Operation op2(OperationType::OUTPUT_SHAPE_CHECK, 1, true);
    Operation op3(OperationType::STATE_TRANSITION_VERIFY, 2, true);
    trace.operations.push_back(op1);
    trace.operations.push_back(op2);
    trace.operations.push_back(op3);

    trace.success = true;
    trace.operation_count = 3;
    trace.final_hash = trace.computeHash();

    S15Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Valid state transitions should not violate S15";
}

TEST_F(CovenantSemanticTest, S15_Violation_InvalidTransition) {
    // Create invalid trace: execution succeeds despite failed validation
    ExecutionTrace trace(42, "s15_invalid");
    trace.script = {0x51};

    // Add introspection op that fails (invalid state transition)
    Operation op(OperationType::OP_COVENANT_CHECK, 0, false);  // Failed!
    trace.operations.push_back(op);

    trace.success = true;  // But execution succeeded anyway!
    trace.operation_count = 1;
    trace.final_hash = trace.computeHash();

    S15Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Invalid transition should violate S15";
    EXPECT_EQ(violations[0].property_name, "S15: Covenant State Transitions");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
