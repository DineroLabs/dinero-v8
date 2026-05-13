#include <gtest/gtest.h>
#include "../properties/economic_liveness_oracle_e6.h"
#include "../properties/economic_liveness_oracle_e7.h"
#include "../properties/economic_liveness_oracle_e8.h"
#include "../properties/economic_liveness_oracle_e9.h"
#include "../properties/economic_liveness_oracle_e10.h"

using namespace dinero::economic::test;

class EconomicLivenessTest : public ::testing::Test {
protected:
    void SetUp() override {
        policy_ = EconomicPolicy();
        policy_.min_relay_fee_una = 1000;
        policy_.dust_threshold_una = 546;
    }

    EconomicPolicy policy_;
};

// ============================================================================
// E6: Fee-Bearing TX Inclusion Tests
// ============================================================================

TEST_F(EconomicLivenessTest, E6_NoViolation_TxConfirmed) {
    EconomicTrace trace;
    trace.policy = policy_;
    trace.end_time = 1000;

    // Transaction accepted
    EconomicEvent accepted;
    accepted.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted.timestamp = 100;
    accepted.node_id = "alice";
    accepted.tx_id = "tx1";
    accepted.success = true;
    trace.events.push_back(accepted);

    // Block template assembled
    EconomicEvent template_event;
    template_event.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    template_event.timestamp = 200;
    template_event.node_id = "alice";
    trace.events.push_back(template_event);

    // Transaction confirmed
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 300;
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx1";
    trace.events.push_back(confirmed);

    E6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Confirmed tx should not violate";
}

TEST_F(EconomicLivenessTest, E6_Violation_TxNeverConfirmed) {
    EconomicTrace trace;
    trace.policy = policy_;
    trace.end_time = 1000;

    // Transaction accepted
    EconomicEvent accepted;
    accepted.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted.timestamp = 100;
    accepted.node_id = "alice";
    accepted.tx_id = "tx_stuck";
    accepted.success = true;
    trace.events.push_back(accepted);

    // Block template assembled (so blocks were mined)
    EconomicEvent template_event;
    template_event.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    template_event.timestamp = 200;
    template_event.node_id = "alice";
    trace.events.push_back(template_event);

    // But tx never confirmed!

    E6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect unconfirmed tx";
    EXPECT_EQ(violations[0].property_name, "E6: Fee-Bearing TX Inclusion");
}

TEST_F(EconomicLivenessTest, E6_NoViolation_NoBlocksMined) {
    EconomicTrace trace;
    trace.policy = policy_;
    trace.end_time = 1000;

    // Transaction accepted
    EconomicEvent accepted;
    accepted.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted.timestamp = 100;
    accepted.node_id = "alice";
    accepted.tx_id = "tx1";
    accepted.success = true;
    trace.events.push_back(accepted);

    // No blocks mined, so no violation expected

    E6Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No blocks mined, no liveness requirement";
}

// ============================================================================
// E7: Mempool Replacement (RBF) Tests
// ============================================================================

TEST_F(EconomicLivenessTest, E7_NoViolation_SuccessfulRBF) {
    EconomicTrace trace;
    trace.policy = policy_;

    // RBF successful
    EconomicEvent rbf;
    rbf.type = EconomicEventType::TX_REPLACED_RBF;
    rbf.timestamp = 200;
    rbf.node_id = "alice";
    rbf.tx_id = "tx_v2";
    rbf.replaced_tx_id = "tx_v1";
    rbf.success = true;
    trace.events.push_back(rbf);

    E7Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Successful RBF should not violate";
}

TEST_F(EconomicLivenessTest, E7_Violation_FailedRBF) {
    EconomicTrace trace;
    trace.policy = policy_;

    // RBF failed
    EconomicEvent rbf;
    rbf.type = EconomicEventType::TX_REPLACED_RBF;
    rbf.timestamp = 200;
    rbf.node_id = "alice";
    rbf.tx_id = "tx_v2";
    rbf.replaced_tx_id = "tx_v1";
    rbf.success = false;
    rbf.error_message = "Fee too low";
    trace.events.push_back(rbf);

    E7Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect failed RBF";
    EXPECT_EQ(violations[0].property_name, "E7: Mempool Replacement (RBF)");
}

TEST_F(EconomicLivenessTest, E7_Violation_OldTxConfirmedAfterRBF) {
    EconomicTrace trace;
    trace.policy = policy_;

    // RBF successful
    EconomicEvent rbf;
    rbf.type = EconomicEventType::TX_REPLACED_RBF;
    rbf.timestamp = 200;
    rbf.node_id = "alice";
    rbf.tx_id = "tx_v2";
    rbf.replaced_tx_id = "tx_v1";
    rbf.success = true;
    trace.events.push_back(rbf);

    // But old transaction confirmed after replacement!
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 300;  // After RBF
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx_v1";  // Old tx
    trace.events.push_back(confirmed);

    E7Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect old tx confirmed after RBF";
}

// ============================================================================
// E8: Fee Estimation Tests
// ============================================================================

TEST_F(EconomicLivenessTest, E8_NoViolation_ValidEstimate) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Valid fee estimate
    EconomicEvent estimate;
    estimate.type = EconomicEventType::FEE_ESTIMATE_UPDATED;
    estimate.timestamp = 100;
    estimate.node_id = "alice";
    estimate.estimated_fee_rate = 10.0;  // 10 sat/byte
    trace.events.push_back(estimate);

    E8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Valid estimate should not violate";
}

TEST_F(EconomicLivenessTest, E8_Violation_NegativeEstimate) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Negative fee estimate
    EconomicEvent estimate;
    estimate.type = EconomicEventType::FEE_ESTIMATE_UPDATED;
    estimate.timestamp = 100;
    estimate.node_id = "alice";
    estimate.estimated_fee_rate = -5.0;  // Negative!
    trace.events.push_back(estimate);

    E8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect negative estimate";
    EXPECT_EQ(violations[0].property_name, "E8: Fee Estimation");
}

TEST_F(EconomicLivenessTest, E8_Violation_UnreasonablyHighEstimate) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Unreasonably high fee estimate
    EconomicEvent estimate;
    estimate.type = EconomicEventType::FEE_ESTIMATE_UPDATED;
    estimate.timestamp = 100;
    estimate.node_id = "alice";
    estimate.estimated_fee_rate = 5000.0;  // 5000 sat/byte, unreasonable!
    trace.events.push_back(estimate);

    E8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect unreasonably high estimate";
}

TEST_F(EconomicLivenessTest, E8_Violation_BelowMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Fee estimate below minimum relay fee
    EconomicEvent estimate;
    estimate.type = EconomicEventType::FEE_ESTIMATE_UPDATED;
    estimate.timestamp = 100;
    estimate.node_id = "alice";
    estimate.estimated_fee_rate = 0.5;  // 0.5 sat/byte, below min relay
    trace.events.push_back(estimate);

    E8Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect estimate below minimum";
}

// ============================================================================
// E9: Block Assembly Tests
// ============================================================================

TEST_F(EconomicLivenessTest, E9_NoViolation_HighFeeFirst) {
    EconomicTrace trace;
    trace.policy = policy_;

    // High-fee tx selected
    EconomicEvent selected_high;
    selected_high.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected_high.timestamp = 100;
    selected_high.node_id = "alice";
    selected_high.tx_id = "tx_high";
    selected_high.fee_rate = 20.0;  // High fee
    trace.events.push_back(selected_high);

    // Low-fee tx excluded
    EconomicEvent excluded_low;
    excluded_low.type = EconomicEventType::TX_EXCLUDED_FROM_BLOCK;
    excluded_low.timestamp = 100;
    excluded_low.node_id = "alice";
    excluded_low.tx_id = "tx_low";
    excluded_low.fee_rate = 5.0;  // Low fee
    trace.events.push_back(excluded_low);

    E9Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "High fee selected, low fee excluded - correct";
}

TEST_F(EconomicLivenessTest, E9_Violation_LowFeeSelected_HighFeeExcluded) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Low-fee tx selected
    EconomicEvent selected_low;
    selected_low.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected_low.timestamp = 100;
    selected_low.node_id = "alice";
    selected_low.tx_id = "tx_low";
    selected_low.fee_rate = 5.0;  // Low fee
    trace.events.push_back(selected_low);

    // High-fee tx excluded!
    EconomicEvent excluded_high;
    excluded_high.type = EconomicEventType::TX_EXCLUDED_FROM_BLOCK;
    excluded_high.timestamp = 100;
    excluded_high.node_id = "alice";
    excluded_high.tx_id = "tx_high";
    excluded_high.fee_rate = 20.0;  // High fee
    trace.events.push_back(excluded_high);

    E9Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect high fee excluded while low fee selected";
    EXPECT_EQ(violations[0].property_name, "E9: Block Assembly");
}

// ============================================================================
// E10: Economic Finality Tests
// ============================================================================

TEST_F(EconomicLivenessTest, E10_NoViolation_NoReorg) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction confirmed
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 200;
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx1";
    confirmed.block_height = 10;
    trace.events.push_back(confirmed);

    // No reorg events

    E10Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No reorg should not violate";
}

TEST_F(EconomicLivenessTest, E10_Violation_TxReorgedOut) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction confirmed
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 200;
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx1";
    confirmed.block_height = 10;
    trace.events.push_back(confirmed);

    // Later reorged out
    EconomicEvent reorg;
    reorg.type = EconomicEventType::TX_REORGED_OUT;
    reorg.timestamp = 400;
    reorg.node_id = "alice";
    reorg.tx_id = "tx1";
    reorg.block_height = 9;
    trace.events.push_back(reorg);

    E10Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect tx reorged out";
    EXPECT_EQ(violations[0].property_name, "E10: Economic Finality");
}

TEST_F(EconomicLivenessTest, E10_NoViolation_DifferentTx) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction 1 confirmed
    EconomicEvent confirmed1;
    confirmed1.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed1.timestamp = 200;
    confirmed1.node_id = "alice";
    confirmed1.tx_id = "tx1";
    trace.events.push_back(confirmed1);

    // Transaction 2 reorged out (different tx)
    EconomicEvent reorg2;
    reorg2.type = EconomicEventType::TX_REORGED_OUT;
    reorg2.timestamp = 400;
    reorg2.node_id = "alice";
    reorg2.tx_id = "tx2";
    trace.events.push_back(reorg2);

    E10Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Different tx reorged, tx1 unaffected";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
