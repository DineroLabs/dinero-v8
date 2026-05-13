#include <gtest/gtest.h>
#include "../properties/consensus_liveness_oracle_dl2.h"
#include "../properties/consensus_liveness_oracle_dl3.h"
#include "../properties/consensus_liveness_oracle_dl4.h"
#include "../properties/consensus_liveness_oracle_dl5.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class LivenessOraclesTest : public ::testing::Test {
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
// DL2: Block Propagation Tests
// ============================================================================

TEST_F(LivenessOraclesTest, DL2_NoViolation_BlockPropagates) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl2_propagates";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Block broadcast at T=100
    ConsensusEvent alice_accept;
    alice_accept.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_accept.node_id = "alice";
    alice_accept.timestamp = 100;
    alice_accept.block_hash = "block_1";
    alice_accept.success = true;
    trace.events.push_back(alice_accept);

    // Bob receives at T=150
    ConsensusEvent bob_receive;
    bob_receive.type = ConsensusEventType::BLOCK_RECEIVED;
    bob_receive.node_id = "bob";
    bob_receive.timestamp = 150;
    bob_receive.block_hash = "block_1";
    trace.events.push_back(bob_receive);

    // Carol receives at T=200
    ConsensusEvent carol_receive;
    carol_receive.type = ConsensusEventType::BLOCK_RECEIVED;
    carol_receive.node_id = "carol";
    carol_receive.timestamp = 200;
    carol_receive.block_hash = "block_1";
    trace.events.push_back(carol_receive);

    // Mark all as honest
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DL2Oracle oracle(500);  // 500ms timeout
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Block reached all nodes within timeout";
}

TEST_F(LivenessOraclesTest, DL2_Violation_BlockDoesNotPropagate) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl2_fails";
    trace.start_time = 0;
    trace.end_time = 1000;

    // Block broadcast at T=100
    ConsensusEvent alice_accept;
    alice_accept.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_accept.node_id = "alice";
    alice_accept.timestamp = 100;
    alice_accept.block_hash = "block_1";
    alice_accept.success = true;
    trace.events.push_back(alice_accept);

    // Bob receives at T=150
    ConsensusEvent bob_receive;
    bob_receive.type = ConsensusEventType::BLOCK_RECEIVED;
    bob_receive.node_id = "bob";
    bob_receive.timestamp = 150;
    bob_receive.block_hash = "block_1";
    trace.events.push_back(bob_receive);

    // Carol NEVER receives (network partition)

    // Mark all as honest
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    DL2Oracle oracle(500);  // Deadline = 100 + 500 = 600
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect propagation failure";
    EXPECT_EQ(violations[0].property_name, "DL2: Block Propagation");
}

// ============================================================================
// DL3: Chain Growth Tests
// ============================================================================

TEST_F(LivenessOraclesTest, DL3_NoViolation_ChainGrows) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl3_grows";
    trace.start_time = 0;
    trace.end_time = 3000;

    // Block at height 1
    ConsensusEvent e1;
    e1.type = ConsensusEventType::BLOCK_ACCEPTED;
    e1.node_id = "alice";
    e1.timestamp = 100;
    e1.block_height = 1;
    trace.events.push_back(e1);

    // Block at height 2
    ConsensusEvent e2;
    e2.type = ConsensusEventType::BLOCK_ACCEPTED;
    e2.node_id = "bob";
    e2.timestamp = 500;
    e2.block_height = 2;
    trace.events.push_back(e2);

    // Final state: height 2
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.chain_height = 2;
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    DL3Oracle oracle(2000);
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Chain grew from 1 to 2";
}

TEST_F(LivenessOraclesTest, DL3_Violation_ChainStalls) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl3_stalls";
    trace.start_time = 0;
    trace.end_time = 5000;

    // Only one block at height 1
    ConsensusEvent e1;
    e1.type = ConsensusEventType::BLOCK_ACCEPTED;
    e1.node_id = "alice";
    e1.timestamp = 100;
    e1.block_height = 1;
    trace.events.push_back(e1);

    // No more blocks - stall!

    // Final state: still height 1
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.chain_height = 1;
    alice_state.is_byzantine = false;
    trace.snapshots.push_back(alice_state);

    DL3Oracle oracle(2000);  // Timeout = 2000ms
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect chain stall";
    EXPECT_EQ(violations[0].property_name, "DL3: Chain Growth");
}

// ============================================================================
// DL4: Transaction Inclusion Tests
// ============================================================================

TEST_F(LivenessOraclesTest, DL4_NoViolation_TxIncluded) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl4_included";
    trace.start_time = 0;
    trace.end_time = 5000;

    // Transaction received at T=100
    ConsensusEvent tx_event;
    tx_event.type = ConsensusEventType::TX_RECEIVED;
    tx_event.node_id = "alice";
    tx_event.timestamp = 100;
    tx_event.tx_id = "tx_hash_123";
    trace.events.push_back(tx_event);

    // Transaction included in block at T=500
    ConsensusEvent block_event;
    block_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    block_event.node_id = "bob";
    block_event.timestamp = 500;
    block_event.tx_id = "tx_hash_123";  // Simplified: tx_id indicates inclusion
    trace.events.push_back(block_event);

    DL4Oracle oracle(3000);
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Transaction included within timeout";
}

TEST_F(LivenessOraclesTest, DL4_Violation_TxNotIncluded) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl4_not_included";
    trace.start_time = 0;
    trace.end_time = 5000;

    // Transaction received at T=100
    ConsensusEvent tx_event;
    tx_event.type = ConsensusEventType::TX_RECEIVED;
    tx_event.node_id = "alice";
    tx_event.timestamp = 100;
    tx_event.tx_id = "tx_hash_123";
    trace.events.push_back(tx_event);

    // No block includes this transaction

    DL4Oracle oracle(3000);  // Deadline = 100 + 3000 = 3100
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect tx not included";
    EXPECT_EQ(violations[0].property_name, "DL4: Transaction Inclusion");
}

// ============================================================================
// DL5: Sync Completion Tests
// ============================================================================

TEST_F(LivenessOraclesTest, DL5_NoViolation_NodeSyncs) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl5_syncs";
    trace.start_time = 0;
    trace.end_time = 10000;

    // Network at height 100 at T=0
    ConsensusEvent network_block;
    network_block.type = ConsensusEventType::BLOCK_ACCEPTED;
    network_block.node_id = "alice";
    network_block.timestamp = 0;
    network_block.block_height = 100;
    trace.events.push_back(network_block);

    // New node (Dave) starts at T=1000
    ConsensusAction start_action;
    start_action.type = ConsensusActionType::NODE_START;
    start_action.node_id = "dave";
    start_action.timestamp = 1000;
    trace.actions.push_back(start_action);

    // Dave reaches height 100 at T=4000
    ConsensusEvent dave_sync;
    dave_sync.type = ConsensusEventType::BLOCK_ACCEPTED;
    dave_sync.node_id = "dave";
    dave_sync.timestamp = 4000;
    dave_sync.block_height = 100;
    trace.events.push_back(dave_sync);

    DL5Oracle oracle(5000);
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Dave synced within 3000ms < 5000ms timeout";
}

TEST_F(LivenessOraclesTest, DL5_Violation_NodeDoesNotSync) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dl5_no_sync";
    trace.start_time = 0;
    trace.end_time = 10000;

    // Network at height 100 at T=0
    ConsensusEvent network_block;
    network_block.type = ConsensusEventType::BLOCK_ACCEPTED;
    network_block.node_id = "alice";
    network_block.timestamp = 0;
    network_block.block_height = 100;
    trace.events.push_back(network_block);

    // New node (Dave) starts at T=1000
    ConsensusAction start_action;
    start_action.type = ConsensusActionType::NODE_START;
    start_action.node_id = "dave";
    start_action.timestamp = 1000;
    trace.actions.push_back(start_action);

    // Dave only reaches height 50 by T=10000 (didn't complete sync)
    ConsensusState dave_state;
    dave_state.node_id = "dave";
    dave_state.timestamp = 10000;
    dave_state.chain_height = 50;
    dave_state.is_byzantine = false;
    trace.snapshots.push_back(dave_state);

    DL5Oracle oracle(5000);  // Deadline = 1000 + 5000 = 6000
    auto violations = oracle.check(trace);

    ASSERT_FALSE(violations.empty()) << "Should detect sync failure";
    EXPECT_EQ(violations[0].property_name, "DL5: Sync Completion");
}

// ============================================================================
// Combined Liveness Check
// ============================================================================

TEST_F(LivenessOraclesTest, AllLivenessPropertiesHold) {
    // Create a well-behaved trace
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "all_liveness";
    trace.start_time = 0;
    trace.end_time = 3000;

    // All nodes converge, blocks propagate, chain grows
    for (const auto& node_id : nodes_) {
        ConsensusState s;
        s.node_id = node_id;
        s.chain_tip_hash = "common_tip";
        s.chain_height = 10;
        s.is_byzantine = false;
        trace.snapshots.push_back(s);
    }

    // Check all liveness properties
    DL2Oracle dl2;
    DL3Oracle dl3;
    DL4Oracle dl4;
    DL5Oracle dl5;

    EXPECT_TRUE(dl2.check(trace).empty()) << "DL2 should pass";
    EXPECT_TRUE(dl3.check(trace).empty()) << "DL3 should pass";
    EXPECT_TRUE(dl4.check(trace).empty()) << "DL4 should pass";
    EXPECT_TRUE(dl5.check(trace).empty()) << "DL5 should pass";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
