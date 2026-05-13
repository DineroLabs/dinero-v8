#include <gtest/gtest.h>
#include "../properties/economic_incentive_oracle_e11.h"
#include "../properties/economic_incentive_oracle_e12.h"
#include "../properties/economic_incentive_oracle_e13.h"
#include "../properties/economic_incentive_oracle_e14.h"
#include "../properties/economic_incentive_oracle_e15.h"

using namespace dinero::economic::test;

class EconomicIncentiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        policy_ = EconomicPolicy();
        policy_.min_relay_fee_una = 1000;
        policy_.dust_threshold_una = 546;
    }

    EconomicPolicy policy_;
};

// ============================================================================
// E11: Mining Incentive Compatibility Tests
// ============================================================================

TEST_F(EconomicIncentiveTest, E11_NoViolation_HighFeeFirst) {
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

    E11Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Miner selected high-fee tx correctly";
}

TEST_F(EconomicIncentiveTest, E11_Violation_LowFeeSelected_HighFeeExcluded) {
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

    E11Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect suboptimal miner behavior";
    EXPECT_EQ(violations[0].property_name, "E11: Mining Incentive Compatibility");
}

// ============================================================================
// E12: Fee Market Efficiency Tests
// ============================================================================

TEST_F(EconomicIncentiveTest, E12_NoViolation_HighFeeConfirmsFirst) {
    EconomicTrace trace;
    trace.policy = policy_;

    // High-fee tx confirmed first
    EconomicEvent confirmed_high;
    confirmed_high.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed_high.timestamp = 200;
    confirmed_high.node_id = "alice";
    confirmed_high.tx_id = "tx_high";
    confirmed_high.fee_rate = 20.0;  // High fee
    trace.events.push_back(confirmed_high);

    // Low-fee tx confirmed later
    EconomicEvent confirmed_low;
    confirmed_low.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed_low.timestamp = 300;
    confirmed_low.node_id = "alice";
    confirmed_low.tx_id = "tx_low";
    confirmed_low.fee_rate = 5.0;  // Low fee
    trace.events.push_back(confirmed_low);

    E12Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Fee market is efficient";
}

TEST_F(EconomicIncentiveTest, E12_Violation_LowFeeConfirmsFirst) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Low-fee tx confirmed first
    EconomicEvent confirmed_low;
    confirmed_low.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed_low.timestamp = 200;
    confirmed_low.node_id = "alice";
    confirmed_low.tx_id = "tx_low";
    confirmed_low.fee_rate = 5.0;  // Low fee
    trace.events.push_back(confirmed_low);

    // High-fee tx confirmed later!
    EconomicEvent confirmed_high;
    confirmed_high.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed_high.timestamp = 300;
    confirmed_high.node_id = "alice";
    confirmed_high.tx_id = "tx_high";
    confirmed_high.fee_rate = 20.0;  // High fee
    trace.events.push_back(confirmed_high);

    E12Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect inefficient fee market";
    EXPECT_EQ(violations[0].property_name, "E12: Fee Market Efficiency");
}

TEST_F(EconomicIncentiveTest, E12_NoViolation_EqualFees) {
    EconomicTrace trace;
    trace.policy = policy_;

    // First tx confirmed first
    EconomicEvent confirmed1;
    confirmed1.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed1.timestamp = 200;
    confirmed1.node_id = "alice";
    confirmed1.tx_id = "tx1";
    confirmed1.fee_rate = 10.0;
    trace.events.push_back(confirmed1);

    // Second tx confirmed later (same fee)
    EconomicEvent confirmed2;
    confirmed2.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed2.timestamp = 300;
    confirmed2.node_id = "alice";
    confirmed2.tx_id = "tx2";
    confirmed2.fee_rate = 10.0;  // Same fee
    trace.events.push_back(confirmed2);

    E12Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Equal fees, order doesn't matter";
}

// ============================================================================
// E13: MEV Resistance Tests
// ============================================================================

TEST_F(EconomicIncentiveTest, E13_NoViolation_ArrivalOrder) {
    EconomicTrace trace;
    trace.policy = policy_;

    // tx1 arrives first
    EconomicEvent accepted1;
    accepted1.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted1.timestamp = 100;
    accepted1.node_id = "alice";
    accepted1.tx_id = "tx1";
    accepted1.fee_rate = 10.0;
    accepted1.success = true;
    trace.events.push_back(accepted1);

    // tx2 arrives later
    EconomicEvent accepted2;
    accepted2.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted2.timestamp = 200;
    accepted2.node_id = "alice";
    accepted2.tx_id = "tx2";
    accepted2.fee_rate = 10.0;  // Same fee
    accepted2.success = true;
    trace.events.push_back(accepted2);

    // tx1 selected first (maintains arrival order)
    EconomicEvent selected1;
    selected1.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected1.timestamp = 300;
    selected1.node_id = "alice";
    selected1.tx_id = "tx1";
    trace.events.push_back(selected1);

    // tx2 selected second
    EconomicEvent selected2;
    selected2.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected2.timestamp = 300;
    selected2.node_id = "alice";
    selected2.tx_id = "tx2";
    trace.events.push_back(selected2);

    E13Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Arrival order maintained for equal fees";
}

TEST_F(EconomicIncentiveTest, E13_Violation_Reordering) {
    EconomicTrace trace;
    trace.policy = policy_;

    // tx1 arrives first
    EconomicEvent accepted1;
    accepted1.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted1.timestamp = 100;
    accepted1.node_id = "alice";
    accepted1.tx_id = "tx1";
    accepted1.fee_rate = 10.0;
    accepted1.success = true;
    trace.events.push_back(accepted1);

    // tx2 arrives later
    EconomicEvent accepted2;
    accepted2.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted2.timestamp = 200;
    accepted2.node_id = "alice";
    accepted2.tx_id = "tx2";
    accepted2.fee_rate = 10.0;  // Same fee
    accepted2.success = true;
    trace.events.push_back(accepted2);

    // tx2 selected first (reordering!)
    EconomicEvent selected2;
    selected2.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected2.timestamp = 300;
    selected2.node_id = "alice";
    selected2.tx_id = "tx2";
    trace.events.push_back(selected2);

    // tx1 selected second
    EconomicEvent selected1;
    selected1.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected1.timestamp = 300;
    selected1.node_id = "alice";
    selected1.tx_id = "tx1";
    trace.events.push_back(selected1);

    E13Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect MEV-like reordering";
    EXPECT_EQ(violations[0].property_name, "E13: MEV Resistance");
}

TEST_F(EconomicIncentiveTest, E13_NoViolation_FeeJustified) {
    EconomicTrace trace;
    trace.policy = policy_;

    // tx1 arrives first (low fee)
    EconomicEvent accepted1;
    accepted1.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted1.timestamp = 100;
    accepted1.node_id = "alice";
    accepted1.tx_id = "tx1";
    accepted1.fee_rate = 5.0;  // Low fee
    accepted1.success = true;
    trace.events.push_back(accepted1);

    // tx2 arrives later (high fee)
    EconomicEvent accepted2;
    accepted2.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted2.timestamp = 200;
    accepted2.node_id = "alice";
    accepted2.tx_id = "tx2";
    accepted2.fee_rate = 20.0;  // High fee
    accepted2.success = true;
    trace.events.push_back(accepted2);

    // tx2 selected first (justified by fee difference)
    EconomicEvent selected2;
    selected2.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected2.timestamp = 300;
    selected2.node_id = "alice";
    selected2.tx_id = "tx2";
    trace.events.push_back(selected2);

    // tx1 selected second
    EconomicEvent selected1;
    selected1.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
    selected1.timestamp = 300;
    selected1.node_id = "alice";
    selected1.tx_id = "tx1";
    trace.events.push_back(selected1);

    E13Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Reordering justified by fee difference";
}

// ============================================================================
// E14: Spam Prevention Tests
// ============================================================================

TEST_F(EconomicIncentiveTest, E14_NoViolation_LowFeeEvicted) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Low-fee tx accepted
    EconomicEvent accepted_low;
    accepted_low.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted_low.timestamp = 100;
    accepted_low.node_id = "alice";
    accepted_low.tx_id = "tx_low";
    accepted_low.fee_rate = 5.0;
    accepted_low.success = true;
    trace.events.push_back(accepted_low);

    // High-fee tx accepted
    EconomicEvent accepted_high;
    accepted_high.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted_high.timestamp = 200;
    accepted_high.node_id = "alice";
    accepted_high.tx_id = "tx_high";
    accepted_high.fee_rate = 20.0;
    accepted_high.success = true;
    trace.events.push_back(accepted_high);

    // Low-fee tx evicted (correct)
    EconomicEvent evicted_low;
    evicted_low.type = EconomicEventType::TX_EVICTED_MEMPOOL;
    evicted_low.timestamp = 300;
    evicted_low.node_id = "alice";
    evicted_low.tx_id = "tx_low";
    evicted_low.fee_rate = 5.0;
    trace.events.push_back(evicted_low);

    E14Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Low-fee tx evicted correctly";
}

TEST_F(EconomicIncentiveTest, E14_Violation_HighFeeEvicted) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Low-fee tx accepted
    EconomicEvent accepted_low;
    accepted_low.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted_low.timestamp = 100;
    accepted_low.node_id = "alice";
    accepted_low.tx_id = "tx_low";
    accepted_low.fee_rate = 5.0;
    accepted_low.success = true;
    trace.events.push_back(accepted_low);

    // High-fee tx accepted
    EconomicEvent accepted_high;
    accepted_high.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted_high.timestamp = 200;
    accepted_high.node_id = "alice";
    accepted_high.tx_id = "tx_high";
    accepted_high.fee_rate = 20.0;
    accepted_high.success = true;
    trace.events.push_back(accepted_high);

    // High-fee tx evicted (wrong!)
    EconomicEvent evicted_high;
    evicted_high.type = EconomicEventType::TX_EVICTED_MEMPOOL;
    evicted_high.timestamp = 300;
    evicted_high.node_id = "alice";
    evicted_high.tx_id = "tx_high";
    evicted_high.fee_rate = 20.0;
    trace.events.push_back(evicted_high);

    // Low-fee tx remains (not evicted, not confirmed)

    E14Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect spam crowding out legitimate tx";
    EXPECT_EQ(violations[0].property_name, "E14: Spam Prevention");
}

// ============================================================================
// E15: Economic DoS Resistance Tests
// ============================================================================

TEST_F(EconomicIncentiveTest, E15_NoViolation_AboveMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee above minimum
    EconomicEvent accepted;
    accepted.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted.timestamp = 100;
    accepted.node_id = "alice";
    accepted.tx_id = "tx1";
    accepted.fee_rate = 2.0;  // Above minimum (1.0)
    accepted.success = true;
    trace.events.push_back(accepted);

    E15Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Fee above minimum";
}

TEST_F(EconomicIncentiveTest, E15_Violation_BelowMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee below minimum
    EconomicEvent accepted;
    accepted.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted.timestamp = 100;
    accepted.node_id = "alice";
    accepted.tx_id = "tx1";
    accepted.fee_rate = 0.5;  // Below minimum (1.0)
    accepted.success = true;
    trace.events.push_back(accepted);

    E15Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect sub-minimum fee";
    EXPECT_EQ(violations[0].property_name, "E15: Economic DoS Resistance");
}

TEST_F(EconomicIncentiveTest, E15_NoViolation_ExactlyMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee exactly at minimum
    EconomicEvent accepted;
    accepted.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    accepted.timestamp = 100;
    accepted.node_id = "alice";
    accepted.tx_id = "tx1";
    accepted.fee_rate = 1.0;  // Exactly minimum
    accepted.success = true;
    trace.events.push_back(accepted);

    E15Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Fee exactly at minimum is acceptable";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
