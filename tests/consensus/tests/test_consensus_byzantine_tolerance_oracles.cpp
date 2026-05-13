#include <gtest/gtest.h>
#include "../properties/consensus_byzantine_tolerance_oracle_db1.h"
#include "../properties/consensus_byzantine_tolerance_oracle_db2.h"
#include "../properties/consensus_byzantine_tolerance_oracle_db3.h"
#include "../properties/consensus_byzantine_tolerance_oracle_db4.h"
#include "../properties/consensus_byzantine_tolerance_oracle_db5.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class ByzantineToleranceTest : public ::testing::Test {
protected:
    void SetUp() override {
        nodes_ = {"alice", "bob", "carol", "eve"};  // eve will be Byzantine
        topology_ = NetworkTopology::fullMesh(nodes_);
        params_ = dinero::ChainParams::regtest();
    }

    std::vector<NodeID> nodes_;
    NetworkTopology topology_;
    dinero::ChainParams params_;
};

// ============================================================================
// DB1: Network Resilience Tests
// ============================================================================

TEST_F(ByzantineToleranceTest, DB1_NoViolation_ProgressDespiteByzantine) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db1_progress";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 1000;

    // Eve becomes Byzantine at T=50
    ConsensusState eve_byzantine;
    eve_byzantine.node_id = "eve";
    eve_byzantine.timestamp = 50;
    eve_byzantine.is_byzantine = true;
    eve_byzantine.chain_height = 0;
    trace.snapshots.push_back(eve_byzantine);

    // Alice mines block after Byzantine node appears
    ConsensusEvent alice_block;
    alice_block.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_block.node_id = "alice";
    alice_block.timestamp = 100;
    alice_block.block_hash = "block_1";
    alice_block.block_height = 1;
    alice_block.success = true;
    trace.events.push_back(alice_block);

    // Mark other nodes as honest
    for (const auto& node_id : std::vector<NodeID>{"alice", "bob", "carol"}) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.is_byzantine = false;
        s.chain_height = 1;
        trace.snapshots.push_back(s);
    }

    DB1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Network made progress despite Byzantine node";
}

TEST_F(ByzantineToleranceTest, DB1_Violation_NetworkStalled) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db1_stall";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 1000;

    // Eve becomes Byzantine at T=50
    ConsensusState eve_byzantine;
    eve_byzantine.node_id = "eve";
    eve_byzantine.timestamp = 50;
    eve_byzantine.is_byzantine = true;
    trace.snapshots.push_back(eve_byzantine);

    // NO blocks produced after Byzantine node appeared

    // Mark other nodes as honest
    for (const auto& node_id : std::vector<NodeID>{"alice", "bob", "carol"}) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DB1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect network stall with Byzantine node";
    EXPECT_EQ(violations[0].property_name, "DB1: Network Resilience");
}

TEST_F(ByzantineToleranceTest, DB1_NoViolation_NoByzantineNodes) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db1_no_byzantine";
    trace.topology = topology_;

    // All nodes honest
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DB1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No Byzantine nodes, property trivially holds";
}

// ============================================================================
// DB2: Eclipse Resistance Tests
// ============================================================================

TEST_F(ByzantineToleranceTest, DB2_NoViolation_HonestNodesConverge) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db2_converge";
    trace.topology = topology_;
    trace.end_time = 1000;

    // Eve is Byzantine
    ConsensusState eve_state;
    eve_state.node_id = "eve";
    eve_state.timestamp = 1000;
    eve_state.is_byzantine = true;
    eve_state.chain_tip_hash = "fake_chain";
    eve_state.chain_height = 10;
    trace.snapshots.push_back(eve_state);

    // All honest nodes converge to same chain
    for (const auto& node_id : std::vector<NodeID>{"alice", "bob", "carol"}) {
        ConsensusState s;
        s.node_id = node_id;
        s.timestamp = 1000;
        s.is_byzantine = false;
        s.chain_tip_hash = "real_chain_tip";
        s.chain_height = 10;
        trace.snapshots.push_back(s);
    }

    DB2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Honest nodes converged despite Byzantine node";
}

TEST_F(ByzantineToleranceTest, DB2_Violation_HonestNodesDiverge) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db2_diverge";
    trace.topology = topology_;
    trace.end_time = 1000;

    // Eve is Byzantine
    ConsensusState eve_state;
    eve_state.node_id = "eve";
    eve_state.timestamp = 1000;
    eve_state.is_byzantine = true;
    eve_state.chain_tip_hash = "fake_chain";
    eve_state.chain_height = 10;
    trace.snapshots.push_back(eve_state);

    // Alice and Bob disagree (eclipse attack succeeded)
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.timestamp = 1000;
    alice_state.is_byzantine = false;
    alice_state.chain_tip_hash = "chain_a";
    alice_state.chain_height = 10;
    trace.snapshots.push_back(alice_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.timestamp = 1000;
    bob_state.is_byzantine = false;
    bob_state.chain_tip_hash = "chain_b";  // DIFFERENT!
    bob_state.chain_height = 10;
    trace.snapshots.push_back(bob_state);

    DB2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect honest node divergence (eclipse attack)";
    EXPECT_EQ(violations[0].property_name, "DB2: Eclipse Resistance");
}

// ============================================================================
// DB3: Double-Spend Resistance Tests
// ============================================================================

TEST_F(ByzantineToleranceTest, DB3_NoViolation_OnlyOneTxConfirmed) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db3_one_confirmed";
    trace.topology = topology_;

    // tx1 accepted
    ConsensusEvent tx1_accepted;
    tx1_accepted.type = ConsensusEventType::TX_ACCEPTED;
    tx1_accepted.node_id = "alice";
    tx1_accepted.timestamp = 100;
    tx1_accepted.tx_id = "tx1";
    tx1_accepted.success = true;
    trace.events.push_back(tx1_accepted);

    // tx2 rejected (conflicts with tx1)
    ConsensusEvent tx2_rejected;
    tx2_rejected.type = ConsensusEventType::TX_REJECTED;
    tx2_rejected.node_id = "alice";
    tx2_rejected.timestamp = 110;
    tx2_rejected.tx_id = "tx2";
    tx2_rejected.success = false;
    tx2_rejected.error_message = "tx conflicts with existing transaction";
    trace.events.push_back(tx2_rejected);

    DB3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Only one conflicting tx confirmed, no double-spend";
}

TEST_F(ByzantineToleranceTest, DB3_NoViolation_NoConflicts) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db3_no_conflicts";
    trace.topology = topology_;

    // Both txs accepted (no conflicts)
    ConsensusEvent tx1;
    tx1.type = ConsensusEventType::TX_ACCEPTED;
    tx1.node_id = "alice";
    tx1.tx_id = "tx1";
    tx1.success = true;
    trace.events.push_back(tx1);

    ConsensusEvent tx2;
    tx2.type = ConsensusEventType::TX_ACCEPTED;
    tx2.node_id = "bob";
    tx2.tx_id = "tx2";
    tx2.success = true;
    trace.events.push_back(tx2);

    DB3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No conflicts, property trivially holds";
}

// ============================================================================
// DB4: Block Withholding Tolerance Tests
// ============================================================================

TEST_F(ByzantineToleranceTest, DB4_NoViolation_ProgressDespiteWithholding) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db4_progress";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 1000;

    // Eve withholds block at T=50
    ConsensusAction withhold;
    withhold.type = ConsensusActionType::WITHHOLD_BLOCK;
    withhold.timestamp = 50;
    withhold.node_id = "eve";
    withhold.block_hash = "withheld_block";
    trace.actions.push_back(withhold);

    // Alice mines block after withholding (honest node progress)
    ConsensusEvent alice_block;
    alice_block.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_block.node_id = "alice";
    alice_block.timestamp = 100;
    alice_block.block_hash = "block_1";
    alice_block.success = true;
    trace.events.push_back(alice_block);

    // Mark nodes
    for (const auto& node_id : std::vector<NodeID>{"alice", "bob", "carol"}) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    ConsensusState eve_state;
    eve_state.node_id = "eve";
    eve_state.is_byzantine = true;
    trace.snapshots.push_back(eve_state);

    DB4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Network continued despite block withholding";
}

TEST_F(ByzantineToleranceTest, DB4_Violation_NetworkStalledByWithholding) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db4_stall";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 1000;

    // Eve withholds block at T=50
    ConsensusAction withhold;
    withhold.type = ConsensusActionType::WITHHOLD_BLOCK;
    withhold.timestamp = 50;
    withhold.node_id = "eve";
    trace.actions.push_back(withhold);

    // NO blocks produced by honest nodes after withholding

    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = (node_id == "eve");
        trace.snapshots.push_back(s);
    }

    DB4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect network stall due to withholding";
    EXPECT_EQ(violations[0].property_name, "DB4: Block Withholding Tolerance");
}

TEST_F(ByzantineToleranceTest, DB4_NoViolation_NoWithholding) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db4_no_withholding";
    trace.topology = topology_;

    DB4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No withholding, property trivially holds";
}

// ============================================================================
// DB5: Invalid Block Rejection Tests
// ============================================================================

TEST_F(ByzantineToleranceTest, DB5_NoViolation_InvalidBlockRejected) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db5_rejected";
    trace.topology = topology_;

    // Eve creates invalid block
    ConsensusEvent invalid_block;
    invalid_block.type = ConsensusEventType::BLOCK_REJECTED;
    invalid_block.node_id = "alice";
    invalid_block.timestamp = 100;
    invalid_block.block_hash = "invalid_block";
    invalid_block.success = false;
    invalid_block.error_message = "invalid block: validation failed";
    trace.events.push_back(invalid_block);

    // Mark nodes
    for (const auto& node_id : std::vector<NodeID>{"alice", "bob", "carol"}) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    ConsensusState eve_state;
    eve_state.node_id = "eve";
    eve_state.is_byzantine = true;
    trace.snapshots.push_back(eve_state);

    DB5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Invalid block was rejected, no violation";
}

TEST_F(ByzantineToleranceTest, DB5_Violation_HonestNodeAcceptedInvalid) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db5_accepted";
    trace.topology = topology_;

    // Alice rejects invalid block (identifies it as invalid)
    ConsensusEvent alice_reject;
    alice_reject.type = ConsensusEventType::BLOCK_REJECTED;
    alice_reject.node_id = "alice";
    alice_reject.timestamp = 100;
    alice_reject.block_hash = "invalid_block";
    alice_reject.success = false;
    alice_reject.error_message = "invalid block: bad PoW";
    trace.events.push_back(alice_reject);

    // Bob (honest) incorrectly accepts the invalid block
    ConsensusEvent bob_accept;
    bob_accept.type = ConsensusEventType::BLOCK_ACCEPTED;
    bob_accept.node_id = "bob";
    bob_accept.timestamp = 105;
    bob_accept.block_hash = "invalid_block";  // Same block!
    bob_accept.success = true;
    trace.events.push_back(bob_accept);

    // Mark nodes
    for (const auto& node_id : std::vector<NodeID>{"alice", "bob", "carol"}) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    ConsensusState eve_state;
    eve_state.node_id = "eve";
    eve_state.is_byzantine = true;
    trace.snapshots.push_back(eve_state);

    DB5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect honest node accepting invalid block";
    EXPECT_EQ(violations[0].property_name, "DB5: Invalid Block Rejection");
    // Description contains truncated block hash (first 8 chars)
    EXPECT_TRUE(violations[0].description.find("invalid_") != std::string::npos ||
                violations[0].description.find("accepted invalid block") != std::string::npos);
}

TEST_F(ByzantineToleranceTest, DB5_NoViolation_NoInvalidBlocks) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "db5_no_invalid";
    trace.topology = topology_;

    // Only valid blocks
    ConsensusEvent valid_block;
    valid_block.type = ConsensusEventType::BLOCK_ACCEPTED;
    valid_block.node_id = "alice";
    valid_block.block_hash = "valid_block";
    valid_block.success = true;
    trace.events.push_back(valid_block);

    DB5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No invalid blocks, property trivially holds";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
