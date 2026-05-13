#include <gtest/gtest.h>
#include "../properties/consensus_safety_oracle_dc1.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class DC1OracleTest : public ::testing::Test {
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
// Agreement Property Tests
// ============================================================================

TEST_F(DC1OracleTest, NoViolation_AllNodesAgree) {
    ConsensusSimulator sim(topology_, params_, 42, "dc1_no_violation");
    sim.setNetworkLatency(10);
    sim.start();

    // All nodes mine blocks in sequence
    sim.simulateBlockMined("alice", "block_1");
    sim.tick(20);

    sim.simulateBlockMined("bob", "block_2");
    sim.tick(20);

    sim.simulateBlockMined("carol", "block_3");
    sim.tick(20);

    // All nodes should have same chain: genesis → block_1 → block_2 → block_3
    auto trace = sim.getTrace();

    DC1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "All nodes agree, should have no violations";
}

TEST_F(DC1OracleTest, NoViolation_NearTipNotFinalized) {
    ConsensusSimulator sim(topology_, params_, 42, "dc1_near_tip");
    sim.setNetworkLatency(10);
    sim.start();

    // Mine only a few blocks (below finalization threshold)
    sim.simulateBlockMined("alice", "block_1");
    sim.tick(20);

    sim.simulateBlockMined("bob", "block_2");
    sim.tick(20);

    // Only 2 blocks mined, finalization depth = 6
    // Nothing is finalized yet, so no violations possible
    auto trace = sim.getTrace();

    DC1Oracle oracle(6);  // 6 block finalization depth
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Near-tip blocks not finalized, no violations";
}

TEST_F(DC1OracleTest, Violation_NodesDisagreeAtHeight) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dc1_violation";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 1000;

    // Alice accepts block_a at height 10
    ConsensusEvent alice_event;
    alice_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_event.node_id = "alice";
    alice_event.timestamp = 100;
    alice_event.sequence_number = 1;
    alice_event.block_hash = "block_a";
    alice_event.block_height = 10;
    alice_event.success = true;
    trace.events.push_back(alice_event);

    // Bob accepts block_b (different!) at height 10
    ConsensusEvent bob_event;
    bob_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    bob_event.node_id = "bob";
    bob_event.timestamp = 100;
    bob_event.sequence_number = 2;
    bob_event.block_hash = "block_b";  // DIFFERENT!
    bob_event.block_height = 10;
    bob_event.success = true;
    trace.events.push_back(bob_event);

    // Carol agrees with Alice (block_a at height 10)
    ConsensusEvent carol_event;
    carol_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    carol_event.node_id = "carol";
    carol_event.timestamp = 100;
    carol_event.sequence_number = 3;
    carol_event.block_hash = "block_a";
    carol_event.block_height = 10;
    carol_event.success = true;
    trace.events.push_back(carol_event);

    // Add more blocks to reach finalization depth
    for (uint32_t h = 11; h <= 20; h++) {
        // Alice continues on block_a chain
        ConsensusEvent e;
        e.type = ConsensusEventType::BLOCK_ACCEPTED;
        e.node_id = "alice";
        e.timestamp = 100 + (h - 10) * 10;
        e.sequence_number = trace.events.size();
        e.block_hash = "block_" + std::to_string(h) + "_a";
        e.block_height = h;
        e.success = true;
        trace.events.push_back(e);

        // Bob continues on block_b chain
        e.node_id = "bob";
        e.block_hash = "block_" + std::to_string(h) + "_b";
        trace.events.push_back(e);

        // Carol continues on block_a chain
        e.node_id = "carol";
        e.block_hash = "block_" + std::to_string(h) + "_a";
        trace.events.push_back(e);
    }

    // Add snapshots showing nodes are honest
    for (const auto& node_id : nodes_) {
        ConsensusState snapshot;
        snapshot.node_id = node_id;
        snapshot.timestamp = 1000;
        snapshot.chain_height = 20;
        snapshot.is_byzantine = false;  // All honest
        trace.snapshots.push_back(snapshot);
    }

    // Check for violations
    DC1Oracle oracle(6);  // 6 block finalization depth
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect disagreement at finalized heights";
    // We'll see violations at all finalized heights where nodes disagree (10-14)
    EXPECT_GE(violations.size(), 1) << "Should detect at least one disagreement";

    // Check first violation (should be at height 10)
    const auto& v = violations[0];
    EXPECT_EQ(v.property_name, "DC1: Agreement");
    EXPECT_TRUE(v.description.find("height 10") != std::string::npos);
    EXPECT_TRUE(v.description.find("block_a") != std::string::npos);
    EXPECT_TRUE(v.description.find("block_b") != std::string::npos);
}

TEST_F(DC1OracleTest, NoViolation_ByzantineNodeIgnored) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dc1_byzantine_ignored";
    trace.topology = topology_;

    // Alice and Carol agree (both honest)
    ConsensusEvent alice_event;
    alice_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_event.node_id = "alice";
    alice_event.block_hash = "block_a";
    alice_event.block_height = 10;
    alice_event.success = true;
    trace.events.push_back(alice_event);

    ConsensusEvent carol_event = alice_event;
    carol_event.node_id = "carol";
    trace.events.push_back(carol_event);

    // Bob disagrees (Byzantine)
    ConsensusEvent bob_event;
    bob_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    bob_event.node_id = "bob";
    bob_event.block_hash = "block_b";  // Different
    bob_event.block_height = 10;
    bob_event.success = true;
    trace.events.push_back(bob_event);

    // Add finalization blocks for all
    for (uint32_t h = 11; h <= 20; h++) {
        ConsensusEvent e;
        e.type = ConsensusEventType::BLOCK_ACCEPTED;
        e.timestamp = h * 10;
        e.block_height = h;
        e.success = true;

        e.node_id = "alice";
        e.block_hash = "block_" + std::to_string(h) + "_a";
        trace.events.push_back(e);

        e.node_id = "carol";
        trace.events.push_back(e);

        e.node_id = "bob";
        e.block_hash = "block_" + std::to_string(h) + "_b";
        trace.events.push_back(e);
    }

    // Mark Alice and Carol as honest, Bob as Byzantine
    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.is_byzantine = false;
    alice_state.chain_height = 20;
    trace.snapshots.push_back(alice_state);

    ConsensusState carol_state;
    carol_state.node_id = "carol";
    carol_state.is_byzantine = false;
    carol_state.chain_height = 20;
    trace.snapshots.push_back(carol_state);

    ConsensusState bob_state;
    bob_state.node_id = "bob";
    bob_state.is_byzantine = true;  // Byzantine!
    bob_state.chain_height = 20;
    trace.snapshots.push_back(bob_state);

    // Check for violations
    DC1Oracle oracle(6);
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Byzantine nodes should be ignored, no violation";
}

TEST_F(DC1OracleTest, NoViolation_SingleHonestNode) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "dc1_single_node";
    trace.topology = topology_;

    // Only Alice is honest
    ConsensusEvent alice_event;
    alice_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    alice_event.node_id = "alice";
    alice_event.block_hash = "block_a";
    alice_event.block_height = 10;
    alice_event.success = true;
    trace.events.push_back(alice_event);

    ConsensusState alice_state;
    alice_state.node_id = "alice";
    alice_state.is_byzantine = false;
    alice_state.chain_height = 10;
    trace.snapshots.push_back(alice_state);

    // Check for violations
    DC1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Need at least 2 honest nodes to check agreement";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
