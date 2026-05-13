#include <gtest/gtest.h>
#include "../properties/consensus_determinism_oracle_dd1.h"
#include "../properties/consensus_determinism_oracle_dd2.h"
#include "../properties/consensus_determinism_oracle_dd3.h"
#include "../properties/consensus_determinism_oracle_dd4.h"
#include "../properties/consensus_determinism_oracle_dd5.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class DeterminismTest : public ::testing::Test {
protected:
    void SetUp() override {
        nodes_ = {"alice", "bob", "carol"};
        topology_ = NetworkTopology::fullMesh(nodes_);
        params_ = dinero::ChainParams::regtest();
    }

    std::vector<NodeID> nodes_;
    NetworkTopology topology_;
    dinero::ChainParams params_;
};

// ============================================================================
// DD1: Trace Reproducibility Tests
// ============================================================================

TEST_F(DeterminismTest, DD1_NoViolation_SameHashAcrossRuns) {
    std::vector<ConsensusTrace> traces;

    // Run same scenario 3 times with same seed
    for (int run = 0; run < 3; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;
        trace.scenario_name = "dd1_same_hash";
        trace.topology = topology_;
        trace.start_time = 0;
        trace.end_time = 1000;

        // Same events
        ConsensusEvent event;
        event.type = ConsensusEventType::BLOCK_ACCEPTED;
        event.node_id = "alice";
        event.timestamp = 100;
        event.sequence_number = 1;
        event.success = true;
        trace.events.push_back(event);

        // Compute deterministic hash (should be same for same events)
        trace.final_hash = 0xABCD1234;  // Deterministic hash
        traces.push_back(trace);
    }

    DD1Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "Same seed should produce same hash";
}

TEST_F(DeterminismTest, DD1_Violation_DifferentHashes) {
    std::vector<ConsensusTrace> traces;

    // Run 1
    ConsensusTrace trace1;
    trace1.rng_seed = 42;
    trace1.scenario_name = "dd1_diff_hash";
    trace1.final_hash = 0xABCD1234;
    traces.push_back(trace1);

    // Run 2 with different hash (non-deterministic!)
    ConsensusTrace trace2;
    trace2.rng_seed = 42;  // Same seed!
    trace2.scenario_name = "dd1_diff_hash";
    trace2.final_hash = 0xDEADBEEF;  // Different hash!
    traces.push_back(trace2);

    DD1Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_FALSE(violations.empty()) << "Should detect hash mismatch";
    EXPECT_EQ(violations[0].property_name, "DD1: Trace Reproducibility");
}

TEST_F(DeterminismTest, DD1_NoViolation_SingleTrace) {
    std::vector<ConsensusTrace> traces;

    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.final_hash = 0xABCD1234;
    traces.push_back(trace);

    DD1Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "Single trace trivially reproducible";
}

// ============================================================================
// DD2: Message Delivery Determinism Tests
// ============================================================================

TEST_F(DeterminismTest, DD2_NoViolation_SameMessageOrder) {
    std::vector<ConsensusTrace> traces;

    for (int run = 0; run < 2; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;

        // Same message delivery sequence
        ConsensusEvent msg1;
        msg1.type = ConsensusEventType::MESSAGE_DELIVERED;
        msg1.node_id = "alice";
        msg1.timestamp = 100;
        msg1.sequence_number = 1;
        trace.events.push_back(msg1);

        ConsensusEvent msg2;
        msg2.type = ConsensusEventType::MESSAGE_DELIVERED;
        msg2.node_id = "bob";
        msg2.timestamp = 110;
        msg2.sequence_number = 2;
        trace.events.push_back(msg2);

        traces.push_back(trace);
    }

    DD2Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "Same message delivery order";
}

TEST_F(DeterminismTest, DD2_Violation_DifferentMessageOrder) {
    std::vector<ConsensusTrace> traces;

    // Run 1: alice then bob
    ConsensusTrace trace1;
    trace1.rng_seed = 42;

    ConsensusEvent msg1;
    msg1.type = ConsensusEventType::MESSAGE_DELIVERED;
    msg1.node_id = "alice";
    msg1.timestamp = 100;
    msg1.sequence_number = 1;
    trace1.events.push_back(msg1);

    ConsensusEvent msg2;
    msg2.type = ConsensusEventType::MESSAGE_DELIVERED;
    msg2.node_id = "bob";
    msg2.timestamp = 110;
    msg2.sequence_number = 2;
    trace1.events.push_back(msg2);

    traces.push_back(trace1);

    // Run 2: bob then alice (different order!)
    ConsensusTrace trace2;
    trace2.rng_seed = 42;

    ConsensusEvent msg3;
    msg3.type = ConsensusEventType::MESSAGE_DELIVERED;
    msg3.node_id = "bob";  // Different node first!
    msg3.timestamp = 100;
    msg3.sequence_number = 1;
    trace2.events.push_back(msg3);

    ConsensusEvent msg4;
    msg4.type = ConsensusEventType::MESSAGE_DELIVERED;
    msg4.node_id = "alice";
    msg4.timestamp = 110;
    msg4.sequence_number = 2;
    trace2.events.push_back(msg4);

    traces.push_back(trace2);

    DD2Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_FALSE(violations.empty()) << "Should detect message order difference";
    EXPECT_EQ(violations[0].property_name, "DD2: Message Delivery Determinism");
}

// ============================================================================
// DD3: State Convergence Determinism Tests
// ============================================================================

TEST_F(DeterminismTest, DD3_NoViolation_SameFinalState) {
    std::vector<ConsensusTrace> traces;

    for (int run = 0; run < 2; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;

        // Same final state for alice
        ConsensusState state;
        state.node_id = "alice";
        state.timestamp = 1000;
        state.chain_tip_hash = "block_abc";
        state.chain_height = 10;
        state.chainwork = 100;
        state.is_byzantine = false;
        trace.snapshots.push_back(state);

        traces.push_back(trace);
    }

    DD3Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "Same final state across runs";
}

TEST_F(DeterminismTest, DD3_Violation_DifferentFinalState) {
    std::vector<ConsensusTrace> traces;

    // Run 1: alice at height 10
    ConsensusTrace trace1;
    trace1.rng_seed = 42;

    ConsensusState state1;
    state1.node_id = "alice";
    state1.timestamp = 1000;
    state1.chain_tip_hash = "block_abc";
    state1.chain_height = 10;
    state1.chainwork = 100;
    state1.is_byzantine = false;
    trace1.snapshots.push_back(state1);

    traces.push_back(trace1);

    // Run 2: alice at height 11 (different!)
    ConsensusTrace trace2;
    trace2.rng_seed = 42;

    ConsensusState state2;
    state2.node_id = "alice";
    state2.timestamp = 1000;
    state2.chain_tip_hash = "block_def";  // Different!
    state2.chain_height = 11;  // Different!
    state2.chainwork = 110;  // Different!
    state2.is_byzantine = false;
    trace2.snapshots.push_back(state2);

    traces.push_back(trace2);

    DD3Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_FALSE(violations.empty()) << "Should detect state difference";
    EXPECT_EQ(violations[0].property_name, "DD3: State Convergence Determinism");
}

// ============================================================================
// DD4: Reorg Determinism Tests
// ============================================================================

TEST_F(DeterminismTest, DD4_NoViolation_SameReorgSequence) {
    std::vector<ConsensusTrace> traces;

    for (int run = 0; run < 2; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;

        // Same reorg sequence
        ConsensusEvent reorg;
        reorg.type = ConsensusEventType::CHAIN_TIP_CHANGED;
        reorg.node_id = "alice";
        reorg.timestamp = 100;
        reorg.sequence_number = 1;
        reorg.success = true;
        trace.events.push_back(reorg);

        traces.push_back(trace);
    }

    DD4Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "Same reorg sequence across runs";
}

TEST_F(DeterminismTest, DD4_Violation_DifferentReorgCount) {
    std::vector<ConsensusTrace> traces;

    // Run 1: 1 reorg
    ConsensusTrace trace1;
    trace1.rng_seed = 42;

    ConsensusEvent reorg1;
    reorg1.type = ConsensusEventType::CHAIN_TIP_CHANGED;
    reorg1.node_id = "alice";
    reorg1.timestamp = 100;
    reorg1.sequence_number = 1;
    trace1.events.push_back(reorg1);

    traces.push_back(trace1);

    // Run 2: 2 reorgs (different!)
    ConsensusTrace trace2;
    trace2.rng_seed = 42;

    ConsensusEvent reorg2;
    reorg2.type = ConsensusEventType::CHAIN_TIP_CHANGED;
    reorg2.node_id = "alice";
    reorg2.timestamp = 100;
    reorg2.sequence_number = 1;
    trace2.events.push_back(reorg2);

    ConsensusEvent reorg3;
    reorg3.type = ConsensusEventType::CHAIN_TIP_CHANGED;
    reorg3.node_id = "alice";
    reorg3.timestamp = 200;
    reorg3.sequence_number = 2;
    trace2.events.push_back(reorg3);

    traces.push_back(trace2);

    DD4Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_FALSE(violations.empty()) << "Should detect reorg count difference";
    EXPECT_EQ(violations[0].property_name, "DD4: Reorg Determinism");
}

TEST_F(DeterminismTest, DD4_NoViolation_NoReorgs) {
    std::vector<ConsensusTrace> traces;

    for (int run = 0; run < 2; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;
        // No reorg events
        traces.push_back(trace);
    }

    DD4Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "No reorgs, property trivially holds";
}

// ============================================================================
// DD5: Byzantine Determinism Tests
// ============================================================================

TEST_F(DeterminismTest, DD5_NoViolation_SameByzantineActions) {
    std::vector<ConsensusTrace> traces;

    for (int run = 0; run < 2; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;

        // Same Byzantine actions
        ConsensusAction action;
        action.type = ConsensusActionType::WITHHOLD_BLOCK;
        action.timestamp = 100;
        action.sequence_number = 1;
        action.node_id = "eve";
        trace.actions.push_back(action);

        traces.push_back(trace);
    }

    DD5Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "Same Byzantine actions across runs";
}

TEST_F(DeterminismTest, DD5_Violation_DifferentByzantineActions) {
    std::vector<ConsensusTrace> traces;

    // Run 1: Withhold block
    ConsensusTrace trace1;
    trace1.rng_seed = 42;

    ConsensusAction action1;
    action1.type = ConsensusActionType::WITHHOLD_BLOCK;
    action1.timestamp = 100;
    action1.sequence_number = 1;
    action1.node_id = "eve";
    trace1.actions.push_back(action1);

    traces.push_back(trace1);

    // Run 2: Double spend (different!)
    ConsensusTrace trace2;
    trace2.rng_seed = 42;

    ConsensusAction action2;
    action2.type = ConsensusActionType::DOUBLE_SPEND_ATTEMPT;  // Different!
    action2.timestamp = 100;
    action2.sequence_number = 1;
    action2.node_id = "eve";
    trace2.actions.push_back(action2);

    traces.push_back(trace2);

    DD5Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_FALSE(violations.empty()) << "Should detect Byzantine action difference";
    EXPECT_EQ(violations[0].property_name, "DD5: Byzantine Determinism");
}

TEST_F(DeterminismTest, DD5_NoViolation_NoByzantineActions) {
    std::vector<ConsensusTrace> traces;

    for (int run = 0; run < 2; ++run) {
        ConsensusTrace trace;
        trace.rng_seed = 42;
        // No Byzantine actions
        traces.push_back(trace);
    }

    DD5Oracle oracle;
    auto violations = oracle.check(traces);

    EXPECT_TRUE(violations.empty()) << "No Byzantine actions, property trivially holds";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
