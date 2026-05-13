#include <gtest/gtest.h>
#include "../properties/consensus_safety_oracle_dc1.h"
#include "../properties/consensus_safety_oracle_dc2.h"
#include "../properties/consensus_safety_oracle_dc3.h"
#include "../properties/consensus_safety_oracle_dc4.h"
#include "../properties/consensus_safety_oracle_dc5.h"
#include "../framework/consensus_simulator.h"

using namespace dinero::consensus::test;

class SafetyOraclesTest : public ::testing::Test {
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
// DC2: Validity Tests
// ============================================================================

TEST_F(SafetyOraclesTest, DC2_NoViolation_AllBlocksValid) {
    ConsensusSimulator sim(topology_, params_, 42, "dc2_valid");
    sim.start();

    sim.simulateBlockMined("alice", "block_1");
    sim.tick(20);

    auto trace = sim.getTrace();

    DC2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "All blocks valid, no violations";
}

TEST_F(SafetyOraclesTest, DC2_Violation_AcceptedWithFailure) {
    ConsensusTrace trace;
    trace.scenario_name = "dc2_invalid";

    // Alice accepts a block with success=false (shouldn't happen)
    ConsensusEvent event;
    event.type = ConsensusEventType::BLOCK_ACCEPTED;
    event.node_id = "alice";
    event.success = false;  // Invalid!
    trace.events.push_back(event);

    ConsensusState state;
    state.node_id = "alice";
    state.is_byzantine = false;  // Honest
    trace.snapshots.push_back(state);

    DC2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect invalid acceptance";
}

// ============================================================================
// DC3: Integrity Tests
// ============================================================================

TEST_F(SafetyOraclesTest, DC3_NoViolation_NoDoubleSpend) {
    ConsensusSimulator sim(topology_, params_, 42, "dc3_integrity");
    sim.start();

    sim.broadcastTransaction("alice", "tx_1");
    sim.tick(20);

    auto trace = sim.getTrace();

    DC3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No double-spend, no violations";
}

// ============================================================================
// DC4: Total Ordering Tests
// ============================================================================

TEST_F(SafetyOraclesTest, DC4_NoViolation_ConsistentOrdering) {
    ConsensusSimulator sim(topology_, params_, 42, "dc4_ordering");
    sim.start();

    // All nodes mine in same order
    sim.simulateBlockMined("alice", "block_1");
    sim.tick(20);
    sim.simulateBlockMined("bob", "block_2");
    sim.tick(20);

    auto trace = sim.getTrace();

    DC4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Consistent ordering, no violations";
}

// ============================================================================
// DC5: Finality Tests
// ============================================================================

TEST_F(SafetyOraclesTest, DC5_NoViolation_NoDeepReorgs) {
    ConsensusSimulator sim(topology_, params_, 42, "dc5_finality");
    sim.start();

    // Mine blocks without reorgs
    sim.simulateBlockMined("alice", "block_1");
    sim.tick(20);
    sim.simulateBlockMined("bob", "block_2");
    sim.tick(20);

    auto trace = sim.getTrace();

    DC5Oracle oracle(6);
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No deep reorgs, no violations";
}

TEST_F(SafetyOraclesTest, DC5_Violation_DeepReorg) {
    ConsensusTrace trace;
    trace.scenario_name = "dc5_deep_reorg";

    // Alice sees a deep reorg (height drops from 20 to 10)
    ConsensusEvent event1;
    event1.type = ConsensusEventType::CHAIN_TIP_CHANGED;
    event1.node_id = "alice";
    event1.block_height = 20;
    event1.timestamp = 100;
    trace.events.push_back(event1);

    ConsensusEvent event2;
    event2.type = ConsensusEventType::CHAIN_TIP_CHANGED;
    event2.node_id = "alice";
    event2.block_height = 10;  // Dropped 10 blocks!
    event2.timestamp = 200;
    trace.events.push_back(event2);

    ConsensusState state;
    state.node_id = "alice";
    state.is_byzantine = false;  // Honest
    trace.snapshots.push_back(state);

    DC5Oracle oracle(6);  // Finality depth = 6
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect deep reorg (10 blocks > 6)";
}

// ============================================================================
// Combined Safety Test
// ============================================================================

TEST_F(SafetyOraclesTest, AllSafetyProperties_HonestNetwork) {
    ConsensusSimulator sim(topology_, params_, 42, "all_safety");
    sim.setNetworkLatency(10);
    sim.start();

    // All nodes mine blocks cooperatively
    for (int i = 1; i <= 10; i++) {
        std::string miner = nodes_[i % nodes_.size()];
        std::string block_hash = "block_" + std::to_string(i);
        sim.simulateBlockMined(miner, block_hash);
        sim.tick(20);
    }

    auto trace = sim.getTrace();

    // Check all safety properties
    DC1Oracle dc1;
    DC2Oracle dc2;
    DC3Oracle dc3;
    DC4Oracle dc4;
    DC5Oracle dc5;

    EXPECT_TRUE(dc1.check(trace).empty()) << "DC1: Agreement should hold";
    EXPECT_TRUE(dc2.check(trace).empty()) << "DC2: Validity should hold";
    EXPECT_TRUE(dc3.check(trace).empty()) << "DC3: Integrity should hold";
    EXPECT_TRUE(dc4.check(trace).empty()) << "DC4: Total Ordering should hold";
    EXPECT_TRUE(dc5.check(trace).empty()) << "DC5: Finality should hold";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
