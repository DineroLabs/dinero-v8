#include <gtest/gtest.h>
#include "../framework/economic_simulator.h"
#include "../framework/economic_trace.h"

using namespace dinero::economic::test;

class EconomicSimulatorSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        nodes_ = {"alice"};
        policy_ = EconomicPolicy();  // Default policy
        rng_seed_ = 42;
    }

    std::vector<NodeID> nodes_;
    EconomicPolicy policy_;
    uint64_t rng_seed_;
};

// ============================================================================
// Phase 6a Smoke Tests
// ============================================================================

TEST_F(EconomicSimulatorSmokeTest, SimulatorCreation) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    EXPECT_EQ(sim.getNodes().size(), 1);
    EXPECT_EQ(sim.getNodes()[0], "alice");
    EXPECT_EQ(sim.getCurrentTime(), 0);
    EXPECT_EQ(sim.getChainHeight("alice"), 0);
}

TEST_F(EconomicSimulatorSmokeTest, SubmitTransaction_Accepted) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Submit transaction with valid fee
    EconomicAction action;
    action.type = EconomicActionType::SUBMIT_TX;
    action.node_id = "alice";
    action.tx_id = "tx1";
    action.fee_una = 5000;      // 5000 sats
    action.tx_size_bytes = 250;      // 250 bytes
    action.input_value = 105000;
    action.output_value = 100000;

    auto events = sim.executeAction(action);

    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].type, EconomicEventType::TX_ACCEPTED_TO_MEMPOOL);
    EXPECT_TRUE(events[0].success);
    EXPECT_EQ(events[0].tx_id, "tx1");

    // Verify mempool state
    auto& mempool = sim.getMempool("alice");
    EXPECT_TRUE(mempool.hasTx("tx1"));
    EXPECT_EQ(mempool.getTxCount(), 1);
    EXPECT_EQ(mempool.getTotalFees(), 5000);
}

TEST_F(EconomicSimulatorSmokeTest, SubmitTransaction_RejectedLowFee) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Submit transaction with fee below minimum
    EconomicAction action;
    action.type = EconomicActionType::SUBMIT_TX;
    action.node_id = "alice";
    action.tx_id = "tx_lowfee";
    action.fee_una = 500;       // Below min relay fee (1000)
    action.tx_size_bytes = 250;
    action.input_value = 100500;
    action.output_value = 100000;

    auto events = sim.executeAction(action);

    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].type, EconomicEventType::TX_REJECTED_LOW_FEE);
    EXPECT_FALSE(events[0].success);

    // Verify mempool empty
    auto& mempool = sim.getMempool("alice");
    EXPECT_FALSE(mempool.hasTx("tx_lowfee"));
    EXPECT_EQ(mempool.getTxCount(), 0);
}

TEST_F(EconomicSimulatorSmokeTest, RBF_Replacement) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Submit original transaction
    EconomicAction submit1;
    submit1.type = EconomicActionType::SUBMIT_TX;
    submit1.node_id = "alice";
    submit1.tx_id = "tx_v1";
    submit1.fee_una = 2000;
    submit1.tx_size_bytes = 250;
    submit1.input_value = 102000;
    submit1.output_value = 100000;

    sim.executeAction(submit1);

    EXPECT_TRUE(sim.getMempool("alice").hasTx("tx_v1"));

    // Replace with higher-fee version
    EconomicAction replace;
    replace.type = EconomicActionType::REPLACE_TX_RBF;
    replace.node_id = "alice";
    replace.tx_id = "tx_v2";
    replace.replaces_tx_id = "tx_v1";
    replace.fee_una = 4000;  // Higher fee
    replace.tx_size_bytes = 250;

    auto events = sim.executeAction(replace);

    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].type, EconomicEventType::TX_REPLACED_RBF);
    EXPECT_TRUE(events[0].success);
    EXPECT_EQ(events[0].tx_id, "tx_v2");
    EXPECT_EQ(events[0].replaced_tx_id, "tx_v1");

    // Verify mempool updated
    auto& mempool = sim.getMempool("alice");
    EXPECT_FALSE(mempool.hasTx("tx_v1"));
    EXPECT_TRUE(mempool.hasTx("tx_v2"));
}

TEST_F(EconomicSimulatorSmokeTest, BlockAssembly) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Submit 3 transactions with different fees
    EconomicAction tx1;
    tx1.type = EconomicActionType::SUBMIT_TX;
    tx1.node_id = "alice";
    tx1.tx_id = "tx_high";
    tx1.fee_una = 10000;  // Highest fee
    tx1.tx_size_bytes = 250;
    tx1.input_value = 110000;
    tx1.output_value = 100000;

    EconomicAction tx2;
    tx2.type = EconomicActionType::SUBMIT_TX;
    tx2.node_id = "alice";
    tx2.tx_id = "tx_med";
    tx2.fee_una = 5000;   // Medium fee
    tx2.tx_size_bytes = 250;
    tx2.input_value = 105000;
    tx2.output_value = 100000;

    EconomicAction tx3;
    tx3.type = EconomicActionType::SUBMIT_TX;
    tx3.node_id = "alice";
    tx3.tx_id = "tx_low";
    tx3.fee_una = 2000;   // Lowest fee
    tx3.tx_size_bytes = 250;
    tx3.input_value = 102000;
    tx3.output_value = 100000;

    sim.executeAction(tx1);
    sim.executeAction(tx2);
    sim.executeAction(tx3);

    EXPECT_EQ(sim.getMempool("alice").getTxCount(), 3);

    // Mine block
    EconomicAction mine;
    mine.type = EconomicActionType::MINE_BLOCK;
    mine.node_id = "alice";

    auto events = sim.executeAction(mine);

    // Should have: 3 TX_SELECTED + 3 TX_INCLUDED + 1 TEMPLATE_ASSEMBLED + 1 FEE_ESTIMATE
    EXPECT_GE(events.size(), 3);  // At least template assembly + some tx events

    // Verify transactions removed from mempool
    auto& mempool = sim.getMempool("alice");
    EXPECT_EQ(mempool.getTxCount(), 0);

    // Verify chain height increased
    EXPECT_EQ(sim.getChainHeight("alice"), 1);
}

TEST_F(EconomicSimulatorSmokeTest, FeeEstimation) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Submit and mine several blocks with transactions
    for (int block = 0; block < 5; ++block) {
        // Submit transactions
        for (int i = 0; i < 5; ++i) {
            EconomicAction tx;
            tx.type = EconomicActionType::SUBMIT_TX;
            tx.node_id = "alice";
            tx.tx_id = "tx_" + std::to_string(block) + "_" + std::to_string(i);
            tx.fee_una = 2000 + (i * 500);
            tx.tx_size_bytes = 250;
            tx.input_value = 102000 + (i * 500);
            tx.output_value = 100000;

            sim.executeAction(tx);
        }

        // Mine block
        EconomicAction mine;
        mine.type = EconomicActionType::MINE_BLOCK;
        mine.node_id = "alice";
        sim.executeAction(mine);
    }

    // Check fee estimator has data
    auto& fee_estimator = sim.getFeeEstimator();
    EXPECT_TRUE(fee_estimator.hasSufficientData());
    EXPECT_GE(fee_estimator.getBlockCount(), 5);

    // Get fee estimate
    auto estimate = fee_estimator.estimateFeeRate(6);
    EXPECT_TRUE(estimate.has_value());
    if (estimate) {
        EXPECT_GT(*estimate, 0.0);
    }
}

TEST_F(EconomicSimulatorSmokeTest, TraceExtraction) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Execute some actions
    EconomicAction tx1;
    tx1.type = EconomicActionType::SUBMIT_TX;
    tx1.node_id = "alice";
    tx1.tx_id = "tx1";
    tx1.fee_una = 5000;
    tx1.tx_size_bytes = 250;
    tx1.input_value = 105000;
    tx1.output_value = 100000;

    sim.executeAction(tx1);

    EconomicAction mine;
    mine.type = EconomicActionType::MINE_BLOCK;
    mine.node_id = "alice";
    sim.executeAction(mine);

    // Capture snapshot
    sim.captureSnapshot(100);

    // Extract trace
    auto trace = sim.extractTrace();

    EXPECT_EQ(trace.rng_seed, rng_seed_);
    EXPECT_EQ(trace.nodes.size(), 1);
    EXPECT_EQ(trace.actions.size(), 2);    // SUBMIT_TX + MINE_BLOCK
    EXPECT_GE(trace.events.size(), 2);     // At least submission + some mining events
    EXPECT_EQ(trace.snapshots.size(), 1);  // One snapshot
    EXPECT_GT(trace.final_hash, 0);        // Hash computed
}

TEST_F(EconomicSimulatorSmokeTest, MempoolEviction) {
    // Create policy with small mempool limit
    EconomicPolicy small_policy = policy_;
    small_policy.max_mempool_size_bytes = 1000;  // Only 1000 bytes

    EconomicSimulator sim(nodes_, small_policy, rng_seed_);

    // Submit 5 transactions (250 bytes each = 1250 bytes total)
    for (int i = 0; i < 5; ++i) {
        EconomicAction tx;
        tx.type = EconomicActionType::SUBMIT_TX;
        tx.node_id = "alice";
        tx.tx_id = "tx_" + std::to_string(i);
        tx.fee_una = 1000 + (i * 1000);  // Increasing fees
        tx.tx_size_bytes = 250;
        tx.input_value = 101000 + (i * 1000);
        tx.output_value = 100000;

        sim.executeAction(tx);
    }

    // Mempool should have evicted low-fee txs to stay under limit
    auto& mempool = sim.getMempool("alice");
    EXPECT_LE(mempool.getSize(), small_policy.max_mempool_size_bytes);

    // High-fee transactions should be kept
    EXPECT_TRUE(mempool.hasTx("tx_4"));  // Highest fee
}

TEST_F(EconomicSimulatorSmokeTest, TimeAdvancement) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    EXPECT_EQ(sim.getCurrentTime(), 0);

    EconomicAction advance;
    advance.type = EconomicActionType::ADVANCE_TIME;
    advance.time_delta_ms = 5000;  // 5 seconds

    sim.executeAction(advance);

    EXPECT_EQ(sim.getCurrentTime(), 5000);
}

TEST_F(EconomicSimulatorSmokeTest, Reset) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Execute some actions
    EconomicAction tx;
    tx.type = EconomicActionType::SUBMIT_TX;
    tx.node_id = "alice";
    tx.tx_id = "tx1";
    tx.fee_una = 5000;
    tx.tx_size_bytes = 250;
    tx.input_value = 105000;
    tx.output_value = 100000;

    sim.executeAction(tx);

    EXPECT_GT(sim.getAllEvents().size(), 0);
    EXPECT_GT(sim.getMempool("alice").getTxCount(), 0);

    // Reset
    sim.reset();

    EXPECT_EQ(sim.getAllEvents().size(), 0);
    EXPECT_EQ(sim.getAllActions().size(), 0);
    EXPECT_EQ(sim.getMempool("alice").getTxCount(), 0);
    EXPECT_EQ(sim.getCurrentTime(), 0);
}

// ============================================================================
// Multi-Transaction Scenarios
// ============================================================================

TEST_F(EconomicSimulatorSmokeTest, ComplexScenario_MempoolFlow) {
    EconomicSimulator sim(nodes_, policy_, rng_seed_);

    // Submit 10 transactions
    for (int i = 0; i < 10; ++i) {
        EconomicAction tx;
        tx.type = EconomicActionType::SUBMIT_TX;
        tx.node_id = "alice";
        tx.tx_id = "tx_" + std::to_string(i);
        tx.fee_una = 2000 + (i * 500);
        tx.tx_size_bytes = 250;
        tx.input_value = 102000 + (i * 500);
        tx.output_value = 100000;

        sim.executeAction(tx);
    }

    EXPECT_EQ(sim.getMempool("alice").getTxCount(), 10);

    // RBF one transaction
    EconomicAction rbf;
    rbf.type = EconomicActionType::REPLACE_TX_RBF;
    rbf.node_id = "alice";
    rbf.tx_id = "tx_5_v2";
    rbf.replaces_tx_id = "tx_5";
    rbf.fee_una = 10000;  // Much higher fee
    rbf.tx_size_bytes = 250;

    sim.executeAction(rbf);

    EXPECT_EQ(sim.getMempool("alice").getTxCount(), 10);  // Same count (replaced)

    // Mine block
    EconomicAction mine;
    mine.type = EconomicActionType::MINE_BLOCK;
    mine.node_id = "alice";

    sim.executeAction(mine);

    EXPECT_EQ(sim.getMempool("alice").getTxCount(), 0);  // All confirmed
    EXPECT_EQ(sim.getChainHeight("alice"), 1);

    // Extract trace and verify
    auto trace = sim.extractTrace();
    EXPECT_GT(trace.total_txs_submitted, 0);
    EXPECT_GT(trace.total_txs_accepted, 0);
    EXPECT_GT(trace.total_txs_confirmed, 0);
    EXPECT_GT(trace.total_fees_collected, 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
