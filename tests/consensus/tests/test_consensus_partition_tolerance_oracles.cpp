#include <gtest/gtest.h>
#include "../properties/consensus_partition_tolerance_oracle_dn1.h"
#include "../properties/consensus_partition_tolerance_oracle_dn2.h"
#include "../properties/consensus_partition_tolerance_oracle_dn3.h"
#include "../properties/consensus_partition_tolerance_oracle_dn4.h"
#include "../properties/consensus_partition_tolerance_oracle_dn5.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class PartitionToleranceOraclesTest : public ::testing::Test {
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
// DN1: Partition Tolerance Tests
// ============================================================================

TEST_F(PartitionToleranceOraclesTest, DN1_NoViolation_MajorityMakesProgress) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn1_majority_progress";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition at T=100: {alice, bob} vs {carol}
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    partition_action.node_group = std::vector<NodeID>{"alice", "bob"};  // Majority partition
    trace.actions.push_back(partition_action);

    // Majority partition makes progress (alice mines)
    ConsensusEvent alice_mines;
    alice_mines.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_mines.node_id = "alice";
    alice_mines.timestamp = 200;
    alice_mines.block_height = 10;
    alice_mines.success = true;
    trace.events.push_back(alice_mines);

    // Mark all as honest
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.chain_height = 10;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DN1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Majority partition made progress, no violations";
}

TEST_F(PartitionToleranceOraclesTest, DN1_Violation_TotalNetworkStall) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn1_total_stall";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Establish initial height before partition
    ConsensusEvent initial_block;
    initial_block.type = ConsensusEventType::BLOCK_ACCEPTED;
    initial_block.node_id = "alice";
    initial_block.timestamp = 50;
    initial_block.block_height = 5;
    initial_block.success = true;
    trace.events.push_back(initial_block);

    // Partition at T=100
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    trace.actions.push_back(partition_action);

    // NO blocks produced by ANY node after T=100 (total network stall)

    // Mark all as honest at same height (no progress anywhere)
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.chain_height = 5;  // Same height as before partition
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DN1Oracle oracle;
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect total network stall";
    EXPECT_EQ(violations[0].property_name, "DN1: Network Liveness During Partition");
}

// ============================================================================
// DN2: Minority Stall Tests
// ============================================================================

TEST_F(PartitionToleranceOraclesTest, DN2_NoViolation_MinorityOrphaned) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn2_minority_orphaned";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition at T=100
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    partition_action.node_group = std::vector<NodeID>{"alice", "bob"};  // Majority
    trace.actions.push_back(partition_action);

    // Partition heals at T=500
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // After healing, all nodes converge to same state (minority blocks orphaned)
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.chain_tip_hash = "converged_tip";
        s.chain_height = 10;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DN2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Minority blocks orphaned, all nodes converged";
}

TEST_F(PartitionToleranceOraclesTest, DN2_Violation_FailedConvergenceAfterHealing) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn2_convergence_failure";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition at T=100 - specify both partition groups
    ConsensusAction partition_action1;
    partition_action1.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action1.timestamp = 100;
    partition_action1.node_group = std::vector<NodeID>{"alice", "bob"};
    trace.actions.push_back(partition_action1);

    ConsensusAction partition_action2;
    partition_action2.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action2.timestamp = 100;
    partition_action2.node_group = std::vector<NodeID>{"carol"};
    trace.actions.push_back(partition_action2);

    // Partition heals at T=500
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // After healing, nodes did NOT converge (observable divergence)
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 1000;
    alice_state.chain_tip_hash = "tip_a";
    alice_state.chain_height = 10;
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.timestamp = 1000;
    carol_state.chain_tip_hash = "tip_c";  // Different!
    carol_state.chain_height = 10;
    carol_state.is_byzantine = false;
    trace.snapshots.push_back(carol_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 1000;
    bob_state.chain_tip_hash = "tip_a";
    bob_state.chain_height = 10;
    bob_state.is_byzantine = false;
    trace.snapshots.push_back(bob_state);

    DN2Oracle oracle;
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect convergence failure";
    EXPECT_EQ(violations[0].property_name, "DN2: Minority Stall");
}

// ============================================================================
// DN3: Clean Healing Tests
// ============================================================================

TEST_F(PartitionToleranceOraclesTest, DN3_NoViolation_CleanHealing) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn3_clean_healing";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition and heal
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    trace.actions.push_back(partition_action);

    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // After healing, all nodes at same height (no blocks lost)
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.chain_height = 15;  // All at same height
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DN3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "All nodes at same height, clean healing";
}

TEST_F(PartitionToleranceOraclesTest, DN3_Violation_HeightDivergence) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn3_height_divergence";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition and heal
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    trace.actions.push_back(partition_action);

    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // After healing, nodes at different heights (blocks lost)
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 1000;
    alice_state.chain_height = 15;
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 1000;
    bob_state.chain_height = 12;  // Behind!
    bob_state.is_byzantine = false;
    trace.snapshots.push_back(bob_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.timestamp = 1000;
    carol_state.chain_height = 15;
    carol_state.is_byzantine = false;
    trace.snapshots.push_back(carol_state);

    DN3Oracle oracle;
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect height divergence";
    EXPECT_EQ(violations[0].property_name, "DN3: Clean Healing");
}

// ============================================================================
// DN4: Asynchronous Healing Tests
// ============================================================================

TEST_F(PartitionToleranceOraclesTest, DN4_NoViolation_DeterministicHealing) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn4_deterministic";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition and heal
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    trace.actions.push_back(partition_action);

    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // All nodes converged (deterministic outcome)
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.chain_tip_hash = "deterministic_tip";
        s.chain_height = 15;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DN4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Nodes converged, deterministic healing";
}

TEST_F(PartitionToleranceOraclesTest, DN4_Violation_NonDeterministicOutcome) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn4_non_deterministic";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition and heal
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    trace.actions.push_back(partition_action);

    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // Nodes did NOT converge (non-deterministic)
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 1000;
    alice_state.chain_tip_hash = "tip_a";
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 1000;
    bob_state.chain_tip_hash = "tip_b";  // Different!
    bob_state.is_byzantine = false;
    trace.snapshots.push_back(bob_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.timestamp = 1000;
    carol_state.chain_tip_hash = "tip_c";  // Different!
    carol_state.is_byzantine = false;
    trace.snapshots.push_back(carol_state);

    DN4Oracle oracle;
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect non-deterministic outcome";
    EXPECT_EQ(violations[0].property_name, "DN4: Asynchronous Healing");
}

// ============================================================================
// DN5: Cascading Partitions Tests
// ============================================================================

TEST_F(PartitionToleranceOraclesTest, DN5_NoViolation_MultiplePartitionsConverge) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn5_converges";
    trace.start_time = 0;
    trace.end_time = 2000;

    // First partition cycle
    ConsensusAction partition1;
    partition1.type = ConsensusActionType::PARTITION_NETWORK;
    partition1.timestamp = 100;
    trace.actions.push_back(partition1);

    ConsensusAction heal1;
    heal1.type = ConsensusActionType::HEAL_PARTITION;
    heal1.timestamp = 500;
    trace.actions.push_back(heal1);

    // Second partition cycle
    ConsensusAction partition2;
    partition2.type = ConsensusActionType::PARTITION_NETWORK;
    partition2.timestamp = 700;
    trace.actions.push_back(partition2);

    ConsensusAction heal2;
    heal2.type = ConsensusActionType::HEAL_PARTITION;
    heal2.timestamp = 1200;
    trace.actions.push_back(heal2);

    // After all cycles, nodes converged
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 2000;
        s.chain_tip_hash = "final_converged_tip";
        s.chain_height = 20;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DN5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Nodes converged after cascading partitions";
}

TEST_F(PartitionToleranceOraclesTest, DN5_Violation_CascadingPartitionsDiverge) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dn5_diverges";
    trace.start_time = 0;
    trace.end_time = 2000;

    // Multiple partition cycles
    ConsensusAction partition1;
    partition1.type = ConsensusActionType::PARTITION_NETWORK;
    partition1.timestamp = 100;
    trace.actions.push_back(partition1);

    ConsensusAction heal1;
    heal1.type = ConsensusActionType::HEAL_PARTITION;
    heal1.timestamp = 500;
    trace.actions.push_back(heal1);

    ConsensusAction partition2;
    partition2.type = ConsensusActionType::PARTITION_NETWORK;
    partition2.timestamp = 700;
    trace.actions.push_back(partition2);

    ConsensusAction heal2;
    heal2.type = ConsensusActionType::HEAL_PARTITION;
    heal2.timestamp = 1200;
    trace.actions.push_back(heal2);

    // After all cycles, nodes did NOT converge
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 2000;
    alice_state.chain_tip_hash = "tip_a";
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 2000;
    bob_state.chain_tip_hash = "tip_b";  // Different!
    bob_state.is_byzantine = false;
    trace.snapshots.push_back(bob_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.timestamp = 2000;
    carol_state.chain_tip_hash = "tip_a";
    carol_state.is_byzantine = false;
    trace.snapshots.push_back(carol_state);

    DN5Oracle oracle;
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect cascading partition failure";
    EXPECT_EQ(violations[0].property_name, "DN5: Cascading Partitions");
}

// ============================================================================
// Combined Partition Tolerance Check
// ============================================================================

TEST_F(PartitionToleranceOraclesTest, AllPartitionPropertiesHold) {
    // Create a well-behaved partition/heal scenario
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "all_partition_properties";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Partition
    ConsensusAction partition_action;
    partition_action.type = ConsensusActionType::PARTITION_NETWORK;
    partition_action.timestamp = 100;
    partition_action.node_group = std::vector<NodeID>{"alice", "bob"};
    trace.actions.push_back(partition_action);

    // Majority makes progress
    ConsensusEvent alice_mines;
    alice_mines.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_mines.node_id = "alice";
    alice_mines.timestamp = 200;
    alice_mines.block_height = 10;
    alice_mines.success = true;
    trace.events.push_back(alice_mines);

    // Heal
    ConsensusAction heal_action;
    heal_action.type = ConsensusActionType::HEAL_PARTITION;
    heal_action.timestamp = 500;
    trace.actions.push_back(heal_action);

    // All nodes converged
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.chain_tip_hash = "converged_tip";
        s.chain_height = 10;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    // Check all partition tolerance properties
    DN1Oracle dn1;
    DN2Oracle dn2;
    DN3Oracle dn3;
    DN4Oracle dn4;
    DN5Oracle dn5;

    EXPECT_TRUE(dn1.check(trace).empty()) << "DN1 should pass";
    EXPECT_TRUE(dn2.check(trace).empty()) << "DN2 should pass";
    EXPECT_TRUE(dn3.check(trace).empty()) << "DN3 should pass";
    EXPECT_TRUE(dn4.check(trace).empty()) << "DN4 should pass";
    EXPECT_TRUE(dn5.check(trace).empty()) << "DN5 should pass";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
