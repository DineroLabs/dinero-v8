/**
 * Ring 7 Phase 7e: Composition & State Property Tests (S16-S20)
 *
 * Tests the composition & state oracles that verify multi-input execution.
 *
 * Properties tested:
 * - S16: Multi-Input Isolation - Inputs execute independently
 * - S17: Parallel Execution Safety - Concurrent execution is safe
 * - S18: State Consistency - State updates are consistent
 * - S19: Cross-Input Invariants - Invariants hold across inputs
 * - S20: Composition Determinism - Composed execution is deterministic
 */

#include <gtest/gtest.h>
#include "../properties/composition_state_oracle_s16.h"
#include "../properties/composition_state_oracle_s17.h"
#include "../properties/composition_state_oracle_s18.h"
#include "../properties/composition_state_oracle_s19.h"
#include "../properties/composition_state_oracle_s20.h"
#include "../framework/execution_simulator.h"

using namespace dinero::execution::test;

class CompositionStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        rng = std::make_unique<dinero::p2p::test::PropertyTestRNG>(42);
        simulator = std::make_unique<ExecutionSimulator>(42);
    }

    std::unique_ptr<dinero::p2p::test::PropertyTestRNG> rng;
    std::unique_ptr<ExecutionSimulator> simulator;
};

// ============================================================================
// S16: Multi-Input Isolation Tests
// ============================================================================

TEST_F(CompositionStateTest, S16_NoViolation_SingleInput) {
    // Single input - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s16_single");

    S16Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Single input should not violate S16";
}

TEST_F(CompositionStateTest, S16_NoViolation_MultiInputIsolated) {
    // Create valid trace with multiple isolated inputs
    ExecutionTrace trace(42, "s16_isolated");
    trace.script = {0x51};

    // Add multi-input markers
    ExecutionEvent multi1(ExecutionEventType::MULTI_INPUT_START, 0, 0);
    ExecutionEvent multi2(ExecutionEventType::MULTI_INPUT_START, 1, 1);
    trace.events.push_back(multi1);
    trace.events.push_back(multi2);

    // Add isolation markers
    ExecutionEvent isolated1(ExecutionEventType::INPUT_ISOLATED, 0, 0);
    ExecutionEvent isolated2(ExecutionEventType::INPUT_ISOLATED, 1, 1);
    trace.events.push_back(isolated1);
    trace.events.push_back(isolated2);

    // Add combined success
    ExecutionEvent combined(ExecutionEventType::COMBINED_SUCCESS, 2, 2);
    trace.events.push_back(combined);

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S16Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Isolated inputs should not violate S16";
}

TEST_F(CompositionStateTest, S16_Violation_MissingIsolation) {
    // Create invalid trace: multi-input without isolation markers
    ExecutionTrace trace(42, "s16_no_isolation");
    trace.script = {0x51};

    // Add multi-input markers but NO isolation
    ExecutionEvent multi1(ExecutionEventType::MULTI_INPUT_START, 0, 0);
    ExecutionEvent multi2(ExecutionEventType::MULTI_INPUT_START, 1, 1);
    trace.events.push_back(multi1);
    trace.events.push_back(multi2);

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S16Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Missing isolation should violate S16";
    EXPECT_EQ(violations[0].property_name, "S16: Multi-Input Isolation");
}

// ============================================================================
// S17: Parallel Execution Safety Tests
// ============================================================================

TEST_F(CompositionStateTest, S17_NoViolation_NoParallel) {
    // No parallel execution - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s17_no_parallel");

    S17Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No parallel execution should not violate S17";
}

TEST_F(CompositionStateTest, S17_NoViolation_SafeParallel) {
    // Create valid trace with safe parallel execution
    ExecutionTrace trace(42, "s17_safe");
    trace.script = {0x51};

    // Add concurrent operations (same step)
    Operation op1(OperationType::OP_PUSH, 0, true);
    Operation op2(OperationType::OP_PUSH, 0, true);  // Same step = concurrent
    trace.operations.push_back(op1);
    trace.operations.push_back(op2);

    trace.success = true;
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S17Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Safe parallel execution should not violate S17";
}

TEST_F(CompositionStateTest, S17_Violation_InconsistentParallel) {
    // Create invalid trace: inconsistent concurrent outcomes
    ExecutionTrace trace(42, "s17_inconsistent");
    trace.script = {0x51};

    // Add concurrent operations with different outcomes
    Operation op1(OperationType::OP_PUSH, 0, true);   // Success
    Operation op2(OperationType::OP_PUSH, 0, false);  // Failure - inconsistent!
    trace.operations.push_back(op1);
    trace.operations.push_back(op2);

    trace.success = true;
    trace.operation_count = 2;
    trace.final_hash = trace.computeHash();

    S17Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Inconsistent parallel outcomes should violate S17";
    EXPECT_EQ(violations[0].property_name, "S17: Parallel Execution Safety");
}

// ============================================================================
// S18: State Consistency Tests
// ============================================================================

TEST_F(CompositionStateTest, S18_NoViolation_NoState) {
    // No state updates - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s18_no_state");

    S18Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No state updates should not violate S18";
}

TEST_F(CompositionStateTest, S18_NoViolation_ConsistentState) {
    // Create valid trace with consistent state updates
    ExecutionTrace trace(42, "s18_consistent");
    trace.script = {0x51};

    // Add state update event
    ExecutionEvent state_update(ExecutionEventType::STATE_UPDATED, 0, 0);
    trace.events.push_back(state_update);

    // Set final state (trace already has final_state field)
    trace.final_state.step = 0;
    trace.final_state.script_valid = true;

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S18Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Consistent state should not violate S18";
}

TEST_F(CompositionStateTest, S18_Violation_UnfinalizedState) {
    // Create invalid trace: state updates without finalized trace
    ExecutionTrace trace(42, "s18_unfinalized");
    trace.script = {0x51};

    // Add state update events
    ExecutionEvent update1(ExecutionEventType::STATE_UPDATED, 0, 0);
    trace.events.push_back(update1);

    trace.success = true;
    trace.operation_count = 0;
    // Don't compute hash - trace not finalized (violation!)
    trace.final_hash = 0;

    S18Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Unfinalized state should violate S18";
    EXPECT_EQ(violations[0].property_name, "S18: State Consistency");
}

// ============================================================================
// S19: Cross-Input Invariants Tests
// ============================================================================

TEST_F(CompositionStateTest, S19_NoViolation_SingleInput) {
    // Single input - property trivially holds
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s19_single");

    S19Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Single input should not violate S19";
}

TEST_F(CompositionStateTest, S19_NoViolation_InvariantsHold) {
    // Create valid trace with cross-input invariants holding
    ExecutionTrace trace(42, "s19_invariants");
    trace.script = {0x51};

    // Add multi-input markers
    ExecutionEvent multi1(ExecutionEventType::MULTI_INPUT_START, 0, 0);
    ExecutionEvent multi2(ExecutionEventType::MULTI_INPUT_START, 1, 1);
    trace.events.push_back(multi1);
    trace.events.push_back(multi2);

    // Add isolation markers
    ExecutionEvent isolated1(ExecutionEventType::INPUT_ISOLATED, 0, 0);
    ExecutionEvent isolated2(ExecutionEventType::INPUT_ISOLATED, 1, 1);
    trace.events.push_back(isolated1);
    trace.events.push_back(isolated2);

    // Add script start events
    ExecutionEvent start1(ExecutionEventType::SCRIPT_START, 0, 0);
    ExecutionEvent start2(ExecutionEventType::SCRIPT_START, 1, 1);
    trace.events.push_back(start1);
    trace.events.push_back(start2);

    // Add combined success
    ExecutionEvent combined(ExecutionEventType::COMBINED_SUCCESS, 2, 2);
    trace.events.push_back(combined);

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S19Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Valid invariants should not violate S19";
}

TEST_F(CompositionStateTest, S19_Violation_MissingIsolation) {
    // Create invalid trace: inputs without isolation markers
    ExecutionTrace trace(42, "s19_no_isolation");
    trace.script = {0x51};

    // Add multi-input markers
    ExecutionEvent multi1(ExecutionEventType::MULTI_INPUT_START, 0, 0);
    ExecutionEvent multi2(ExecutionEventType::MULTI_INPUT_START, 1, 1);
    trace.events.push_back(multi1);
    trace.events.push_back(multi2);

    // Don't add isolation markers (violation!)

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S19Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Missing isolation should violate S19";
    EXPECT_EQ(violations[0].property_name, "S19: Cross-Input Invariants");
}

// ============================================================================
// S20: Composition Determinism Tests
// ============================================================================

TEST_F(CompositionStateTest, S20_NoViolation_Deterministic) {
    // Create deterministic trace
    std::vector<uint8_t> script = {0x51};  // OP_1
    WitnessStack witness;

    auto trace = simulator->executeScript(script, witness, "s20_deterministic");

    S20Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Deterministic trace should not violate S20";
}

TEST_F(CompositionStateTest, S20_Violation_NoTraceHash) {
    // Create invalid trace: no trace hash
    ExecutionTrace trace(42, "s20_no_hash");
    trace.script = {0x51};

    trace.success = true;
    trace.operation_count = 0;
    // Don't compute hash (violation!)
    trace.final_hash = 0;

    S20Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Missing trace hash should violate S20";
    EXPECT_EQ(violations[0].property_name, "S20: Composition Determinism");
}

TEST_F(CompositionStateTest, S20_Violation_NoRNGSeed) {
    // Create invalid trace: no RNG seed
    ExecutionTrace trace(0, "s20_no_seed");  // RNG seed = 0
    trace.script = {0x51};

    trace.success = true;
    trace.operation_count = 0;
    trace.final_hash = trace.computeHash();

    S20Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "No RNG seed should violate S20";
    EXPECT_EQ(violations[0].property_name, "S20: Composition Determinism");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
