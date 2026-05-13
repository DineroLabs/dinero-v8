#include <gtest/gtest.h>
#include "../framework/consensus_trace.h"
#include "../framework/consensus_types.h"

using namespace dinero::consensus::test;

class ConsensusTraceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create simple 3-node topology
        nodes_ = {"alice", "bob", "carol"};
        topology_ = NetworkTopology::fullMesh(nodes_);
    }

    std::vector<NodeID> nodes_;
    NetworkTopology topology_;
};

TEST_F(ConsensusTraceTest, EmptyTrace_ComputesHash) {
    ConsensusTrace trace;
    trace.rng_seed = 42;
    trace.scenario_name = "empty_test";
    trace.topology = topology_;
    trace.start_time = 0;
    trace.end_time = 0;
    trace.completed_successfully = true;

    uint64_t hash = trace.computeHash();
    EXPECT_NE(hash, 0) << "Hash should be non-zero";
}

TEST_F(ConsensusTraceTest, DeterministicHash_SameSeedSameHash) {
    ConsensusTrace trace1;
    trace1.rng_seed = 42;
    trace1.scenario_name = "determinism_test";
    trace1.topology = topology_;

    ConsensusTrace trace2;
    trace2.rng_seed = 42;
    trace2.scenario_name = "determinism_test";
    trace2.topology = topology_;

    uint64_t hash1 = trace1.computeHash();
    uint64_t hash2 = trace2.computeHash();

    EXPECT_EQ(hash1, hash2) << "Same trace should produce same hash";
}

TEST_F(ConsensusTraceTest, DifferentSeed_DifferentHash) {
    ConsensusTrace trace1;
    trace1.rng_seed = 42;
    trace1.scenario_name = "seed_test";
    trace1.topology = topology_;

    ConsensusTrace trace2;
    trace2.rng_seed = 99;
    trace2.scenario_name = "seed_test";
    trace2.topology = topology_;

    uint64_t hash1 = trace1.computeHash();
    uint64_t hash2 = trace2.computeHash();

    EXPECT_NE(hash1, hash2) << "Different seeds should produce different hashes";
}

TEST_F(ConsensusTraceTest, GetAllNodes_ReturnsUniqueNodes) {
    ConsensusTrace trace;
    trace.topology = topology_;

    // Add some events
    ConsensusEvent event1;
    event1.type = ConsensusEventType::BLOCK_RECEIVED;
    event1.node_id = "alice";
    event1.timestamp = 100;
    trace.events.push_back(event1);

    ConsensusEvent event2;
    event2.type = ConsensusEventType::BLOCK_ACCEPTED;
    event2.node_id = "bob";
    event2.timestamp = 200;
    trace.events.push_back(event2);

    auto all_nodes = trace.getAllNodes();
    EXPECT_GE(all_nodes.size(), 3) << "Should have at least 3 nodes (alice, bob, carol)";
}

TEST_F(ConsensusTraceTest, GetEventsForNode_FiltersCorrectly) {
    ConsensusTrace trace;

    // Add events for different nodes
    ConsensusEvent alice_event;
    alice_event.type = ConsensusEventType::BLOCK_RECEIVED;
    alice_event.node_id = "alice";
    alice_event.timestamp = 100;
    trace.events.push_back(alice_event);

    ConsensusEvent bob_event;
    bob_event.type = ConsensusEventType::BLOCK_ACCEPTED;
    bob_event.node_id = "bob";
    bob_event.timestamp = 200;
    trace.events.push_back(bob_event);

    ConsensusEvent alice_event2;
    alice_event2.type = ConsensusEventType::CHAIN_TIP_CHANGED;
    alice_event2.node_id = "alice";
    alice_event2.timestamp = 300;
    trace.events.push_back(alice_event2);

    auto alice_events = trace.getEventsForNode("alice");
    EXPECT_EQ(alice_events.size(), 2) << "Alice should have 2 events";

    auto bob_events = trace.getEventsForNode("bob");
    EXPECT_EQ(bob_events.size(), 1) << "Bob should have 1 event";

    auto carol_events = trace.getEventsForNode("carol");
    EXPECT_EQ(carol_events.size(), 0) << "Carol should have 0 events";
}

TEST_F(ConsensusTraceTest, NetworkTopology_FullMesh_AllConnected) {
    auto mesh = NetworkTopology::fullMesh(nodes_);

    EXPECT_EQ(mesh.type, TopologyType::FULL_MESH);
    EXPECT_EQ(mesh.nodes.size(), 3);

    // Each node should connect to the other 2
    for (const auto& node : nodes_) {
        EXPECT_EQ(mesh.connections.at(node).size(), 2)
            << node << " should connect to 2 peers in full mesh";
    }
}

TEST_F(ConsensusTraceTest, NetworkTopology_Star_HubAndSpokes) {
    auto star = NetworkTopology::star(nodes_);

    EXPECT_EQ(star.type, TopologyType::STAR);
    EXPECT_EQ(star.nodes.size(), 3);

    // Hub (alice) should connect to 2 spokes
    EXPECT_EQ(star.connections.at("alice").size(), 2);

    // Each spoke should connect only to hub
    EXPECT_EQ(star.connections.at("bob").size(), 1);
    EXPECT_EQ(star.connections.at("bob")[0], "alice");

    EXPECT_EQ(star.connections.at("carol").size(), 1);
    EXPECT_EQ(star.connections.at("carol")[0], "alice");
}

TEST_F(ConsensusTraceTest, NetworkTopology_Chain_LinearConnections) {
    auto chain = NetworkTopology::chain(nodes_);

    EXPECT_EQ(chain.type, TopologyType::CHAIN);
    EXPECT_EQ(chain.nodes.size(), 3);

    // alice (first) connects to bob
    EXPECT_EQ(chain.connections.at("alice").size(), 1);
    EXPECT_EQ(chain.connections.at("alice")[0], "bob");

    // bob (middle) connects to alice and carol
    EXPECT_EQ(chain.connections.at("bob").size(), 2);

    // carol (last) connects to bob
    EXPECT_EQ(chain.connections.at("carol").size(), 1);
    EXPECT_EQ(chain.connections.at("carol")[0], "bob");
}

TEST_F(ConsensusTraceTest, GetStateAt_ReturnsLatestSnapshot) {
    ConsensusTrace trace;

    // Add snapshots at different times
    ConsensusState snapshot1;
    snapshot1.node_id = "alice";
    snapshot1.timestamp = 100;
    snapshot1.chain_height = 10;
    trace.snapshots.push_back(snapshot1);

    ConsensusState snapshot2;
    snapshot2.node_id = "alice";
    snapshot2.timestamp = 200;
    snapshot2.chain_height = 20;
    trace.snapshots.push_back(snapshot2);

    ConsensusState snapshot3;
    snapshot3.node_id = "alice";
    snapshot3.timestamp = 300;
    snapshot3.chain_height = 30;
    trace.snapshots.push_back(snapshot3);

    // Query at timestamp 250 - should get snapshot2 (height=20)
    auto state = trace.getStateAt("alice", 250);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->chain_height, 20);
    EXPECT_EQ(state->timestamp, 200);

    // Query at timestamp 350 - should get snapshot3 (height=30)
    auto state2 = trace.getStateAt("alice", 350);
    ASSERT_TRUE(state2.has_value());
    EXPECT_EQ(state2->chain_height, 30);

    // Query at timestamp 50 - should get nothing (before first snapshot)
    auto state3 = trace.getStateAt("alice", 50);
    EXPECT_FALSE(state3.has_value());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
