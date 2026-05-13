#include <gtest/gtest.h>
#include "../properties/consensus_liveness_oracle_dl1.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class DL1OracleTest : public ::testing::Test {
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
// Eventual Consensus Tests
// ============================================================================

TEST_F(DL1OracleTest, NoViolation_NodesConvergeAfterPartitionHeals) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl1_converge";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 2000;

    // Partition heals at T=500
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // All nodes converge to same tip by T=1000
    for (const auto& node_id : nodes_) {
        ConsensusState snapshot;
        snapshot.node_id = node_id;
        snapshot.timestamp = 1000;
        snapshot.chain_tip_hash = "converged_tip";
        snapshot.chain_height = 10;
        snapshot.is_byzantine = false;
        trace.snapshots.push_back(snapshot);
    }

    DL1Oracle oracle(1000);  // 1000ms convergence timeout
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Nodes converged within timeout, no violations";
}

TEST_F(DL1OracleTest, Violation_NodesFailToConverge) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl1_no_converge";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 2000;

    // Partition heals at T=500
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // Nodes DON'T converge - different tips at T=2000
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 2000;
    alice_state.chain_tip_hash = "tip_a";
    alice_state.chain_height = 10;
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 2000;
    bob_state.chain_tip_hash = "tip_b";  // DIFFERENT!
    bob_state.chain_height = 10;
    bob_state.is_byzantine = false;
    trace.snapshots.push_back(bob_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.timestamp = 2000;
    carol_state.chain_tip_hash = "tip_a";
    carol_state.chain_height = 10;
    carol_state.is_byzantine = false;
    trace.snapshots.push_back(carol_state);

    DL1Oracle oracle(1000);  // Deadline = 500 + 1000 = 1500
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect convergence failure";
    const auto& v = violations[0];
    EXPECT_EQ(v.property_name, "DL1: Eventual Consensus");
    EXPECT_EQ(v.expected_by, 1500);  // heal_time + timeout
}

TEST_F(DL1OracleTest, NoViolation_NoPartitionHeal) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl1_no_partition";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 2000;

    // No partition heal event

    // Nodes have different tips (but no partition heal, so no check)
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 2000;
    alice_state.chain_tip_hash = "tip_a";
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 2000;
    bob_state.chain_tip_hash = "tip_b";
    bob_state.is_byzantine = false;
    trace.snapshots.push_back(bob_state);

    DL1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No partition heal - nothing to check";
}

TEST_F(DL1OracleTest, NoViolation_TraceTooShort) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl1_short_trace";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 1200;  // Ends before deadline

    // Partition heals at T=500
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // Deadline would be 500 + 1000 = 1500, but trace ends at 1200
    // So we can't verify convergence yet

    DL1Oracle oracle(1000);
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Trace too short to verify - not a violation";
}

TEST_F(DL1OracleTest, NoViolation_SingleHonestNode) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl1_single_node";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 2000;

    // Partition heals
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // Only Alice is honest
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 2000;
    alice_state.chain_tip_hash = "tip_a";
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    // Bob and Carol are Byzantine
    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.is_byzantine = true;
    trace.snapshots.push_back(bob_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.is_byzantine = true;
    trace.snapshots.push_back(carol_state);

    DL1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Need at least 2 honest nodes to check convergence";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
