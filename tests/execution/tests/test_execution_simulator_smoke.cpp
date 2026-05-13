/**
 * Ring 7 Phase 7a: Execution Trace Framework Smoke Tests
 *
 * Validates that the execution simulator and trace recording infrastructure
 * works correctly before implementing full semantic properties.
 *
 * Tests:
 * 1. Simple script execution
 * 2. Trace recording
 * 3. Determinism verification
 * 4. Stack operations
 * 5. Opcode trace capture
 */

#include "../framework/execution_simulator.h"
#include "../framework/execution_trace.h"
#include "../framework/execution_types.h"
#include <gtest/gtest.h>

using namespace dinero::execution::test;

//=============================================================================
// Phase 7a Smoke Tests
//=============================================================================

class ExecutionSimulatorSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Fresh RNG for each test
        rng = std::make_unique<dinero::p2p::test::PropertyTestRNG>(42);
    }

    std::unique_ptr<dinero::p2p::test::PropertyTestRNG> rng;
};

//=============================================================================
// Test 1: Simulator Creation
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, SimulatorCreation) {
    ExecutionSimulator simulator(42);
    EXPECT_EQ(simulator.getSeed(), 42);
}

//=============================================================================
// Test 2: Simple Script Execution
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, SimpleScriptExecution) {
    ExecutionSimulator simulator(42);

    // Simple script: OP_1 (pushes 1, succeeds)
    std::vector<uint8_t> script = {0x51};
    WitnessStack witness; // Empty

    auto trace = simulator.executeScript(script, witness, "simple_op1");

    EXPECT_TRUE(trace.wasSuccessful());
    EXPECT_EQ(trace.scenario_name, "simple_op1");
    EXPECT_FALSE(trace.hasFailed());
}

//=============================================================================
// Test 3: Trace Recording
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, TraceRecording) {
    ExecutionSimulator simulator(42);

    // Script: OP_1 OP_2 OP_ADD (should push 1, push 2, add → 3)
    std::vector<uint8_t> script = {0x51, 0x52, 0x93};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "trace_test");

    // Should record operations
    EXPECT_GT(trace.getOperationCount(), 0);

    // Should record stack snapshots
    EXPECT_GT(trace.getStackSnapshotCount(), 0);

    // Should compute final hash
    EXPECT_NE(trace.final_hash, 0);
}

//=============================================================================
// Test 4: Determinism Verification
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, DeterminismVerification) {
    // Same seed → same trace
    uint64_t seed = 123;

    ExecutionSimulator sim1(seed);
    ExecutionSimulator sim2(seed);

    std::vector<uint8_t> script = {0x51, 0x52, 0x93}; // OP_1 OP_2 OP_ADD
    WitnessStack witness;

    auto trace1 = sim1.executeScript(script, witness, "determinism_test");
    auto trace2 = sim2.executeScript(script, witness, "determinism_test");

    // Traces should be equivalent
    EXPECT_TRUE(trace1.isEquivalentTo(trace2));

    // Final hashes should match
    EXPECT_EQ(trace1.final_hash, trace2.final_hash);

    // Operation counts should match
    EXPECT_EQ(trace1.getOperationCount(), trace2.getOperationCount());
}

//=============================================================================
// Test 5: Stack Operations
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, StackOperations_DUP) {
    ExecutionSimulator simulator(42);

    // Script: OP_1 OP_DUP (push 1, duplicate it)
    std::vector<uint8_t> script = {0x51, 0x76};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "stack_dup");

    EXPECT_TRUE(trace.wasSuccessful());

    // Should have recorded DUP operation
    bool found_dup = false;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_DUP) {
            found_dup = true;
            EXPECT_TRUE(op.success);
        }
    }
    EXPECT_TRUE(found_dup);
}

TEST_F(ExecutionSimulatorSmokeTest, StackOperations_SWAP) {
    ExecutionSimulator simulator(42);

    // Script: OP_1 OP_2 OP_SWAP (push 1, push 2, swap)
    std::vector<uint8_t> script = {0x51, 0x52, 0x7c};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "stack_swap");

    EXPECT_TRUE(trace.wasSuccessful());

    // Should have recorded SWAP operation
    bool found_swap = false;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_SWAP) {
            found_swap = true;
            EXPECT_TRUE(op.success);
        }
    }
    EXPECT_TRUE(found_swap);
}

TEST_F(ExecutionSimulatorSmokeTest, StackOperations_DROP) {
    ExecutionSimulator simulator(42);

    // Script: OP_1 OP_2 OP_DROP (push 1, push 2, drop 2)
    std::vector<uint8_t> script = {0x51, 0x52, 0x75};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "stack_drop");

    EXPECT_TRUE(trace.wasSuccessful());

    // Should have recorded DROP operation
    bool found_drop = false;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_DROP) {
            found_drop = true;
            EXPECT_TRUE(op.success);
        }
    }
    EXPECT_TRUE(found_drop);
}

//=============================================================================
// Test 6: Arithmetic Operations
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, ArithmeticOperations_ADD) {
    ExecutionSimulator simulator(42);

    // Script: OP_2 OP_3 OP_ADD (push 2, push 3, add → 5)
    std::vector<uint8_t> script = {0x52, 0x53, 0x93};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "arithmetic_add");

    EXPECT_TRUE(trace.wasSuccessful());

    // Should have recorded ADD operation
    bool found_add = false;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_ADD) {
            found_add = true;
            EXPECT_TRUE(op.success);
        }
    }
    EXPECT_TRUE(found_add);
}

TEST_F(ExecutionSimulatorSmokeTest, ArithmeticOperations_SUB) {
    ExecutionSimulator simulator(42);

    // Script: OP_5 OP_2 OP_SUB (push 5, push 2, sub → 3)
    std::vector<uint8_t> script = {0x55, 0x52, 0x94};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "arithmetic_sub");

    EXPECT_TRUE(trace.wasSuccessful());

    // Should have recorded SUB operation
    bool found_sub = false;
    for (const auto& op : trace.operations) {
        if (op.type == OperationType::OP_SUB) {
            found_sub = true;
            EXPECT_TRUE(op.success);
        }
    }
    EXPECT_TRUE(found_sub);
}

//=============================================================================
// Test 7: Execution Failure Handling
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, ExecutionFailure_StackUnderflow) {
    ExecutionSimulator simulator(42);

    // Script: OP_DROP (drop from empty stack - should fail)
    std::vector<uint8_t> script = {0x75};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "failure_underflow");

    EXPECT_FALSE(trace.wasSuccessful());
    EXPECT_TRUE(trace.hasFailed());
    EXPECT_TRUE(trace.error.has_value());
}

//=============================================================================
// Test 8: Trace Well-Formedness
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, TraceWellFormedness) {
    ExecutionSimulator simulator(42);

    std::vector<uint8_t> script = {0x51, 0x52, 0x93};
    WitnessStack witness;

    auto trace = simulator.executeScript(script, witness, "wellformed_test");

    // Trace should be well-formed
    EXPECT_TRUE(trace.isWellFormed());

    // Operation count should match
    EXPECT_EQ(trace.operation_count, trace.getOperationCount());

    // Final hash should be computed
    EXPECT_NE(trace.final_hash, 0);
}

//=============================================================================
// Test 9: ExecutionSequenceGenerator
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, SequenceGenerator_Simple) {
    ExecutionSequenceGenerator gen(*rng);

    auto trace = gen.generateSimpleScript();

    EXPECT_TRUE(trace.wasSuccessful());
    EXPECT_GT(trace.getOperationCount(), 0);
}

TEST_F(ExecutionSimulatorSmokeTest, SequenceGenerator_Complexity) {
    ExecutionSequenceGenerator gen(*rng);

    // Generate scripts of different complexities
    auto trivial = gen.generateScriptExecution(ScriptComplexity::Trivial);
    auto simple = gen.generateScriptExecution(ScriptComplexity::Simple);
    auto medium = gen.generateScriptExecution(ScriptComplexity::Medium);

    // More complex scripts should have more operations
    EXPECT_LE(trivial.getOperationCount(), simple.getOperationCount());
    EXPECT_LE(simple.getOperationCount(), medium.getOperationCount());
}

TEST_F(ExecutionSimulatorSmokeTest, SequenceGenerator_DeterministicPair) {
    ExecutionSequenceGenerator gen(*rng);

    auto [trace1, trace2] = gen.generateDeterministicPair();

    // Same seed → equivalent traces
    EXPECT_TRUE(trace1.isEquivalentTo(trace2));
    EXPECT_EQ(trace1.final_hash, trace2.final_hash);
}

//=============================================================================
// Test 10: Witness Processing
//=============================================================================

TEST_F(ExecutionSimulatorSmokeTest, WitnessProcessing) {
    ExecutionSimulator simulator(42);

    // Script: just verify stack is populated from witness
    std::vector<uint8_t> script = {0x51}; // OP_1
    WitnessStack witness;
    witness.push({0xAA});
    witness.push({0xBB});

    auto trace = simulator.executeScript(script, witness, "witness_test");

    // Witness elements should be pushed to stack
    EXPECT_EQ(trace.witness.elements.size(), 2);
    EXPECT_EQ(trace.witness.elements[0][0], 0xAA);
    EXPECT_EQ(trace.witness.elements[1][0], 0xBB);
}

//=============================================================================
// Main
//=============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
