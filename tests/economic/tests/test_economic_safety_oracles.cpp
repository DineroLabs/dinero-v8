#include <gtest/gtest.h>
#include "../properties/economic_safety_oracle_e1.h"
#include "../properties/economic_safety_oracle_e2.h"
#include "../properties/economic_safety_oracle_e3.h"
#include "../properties/economic_safety_oracle_e4.h"
#include "../properties/economic_safety_oracle_e5.h"

using namespace dinero::economic::test;

class EconomicSafetyTest : public ::testing::Test {
protected:
    void SetUp() override {
        policy_ = EconomicPolicy();
        policy_.min_relay_fee_una = 1000;
        policy_.dust_threshold_una = 546;
    }

    EconomicPolicy policy_;
};

// ============================================================================
// E1: Fee Validation Tests
// ============================================================================

TEST_F(EconomicSafetyTest, E1_NoViolation_ValidFee) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with valid fee
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 100;
    event.node_id = "alice";
    event.tx_id = "tx1";
    event.fee_una = 5000;
    event.input_value = 105000;
    event.output_value = 100000;
    event.success = true;
    trace.events.push_back(event);

    E1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Valid fee should not violate";
}

TEST_F(EconomicSafetyTest, E1_Violation_FeeMismatch) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee mismatch
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 100;
    event.node_id = "alice";
    event.tx_id = "tx_bad";
    event.fee_una = 10000;  // Claims 10000
    event.input_value = 105000;
    event.output_value = 100000;  // But actual fee is 5000
    event.success = true;
    trace.events.push_back(event);

    E1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect fee mismatch";
    EXPECT_EQ(violations[0].property_name, "E1: Fee Validation");
}

TEST_F(EconomicSafetyTest, E1_Violation_NegativeFee) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with outputs > inputs
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 100;
    event.node_id = "alice";
    event.tx_id = "tx_negative";
    event.fee_una = 0;
    event.input_value = 100000;
    event.output_value = 105000;  // More outputs than inputs!
    event.success = true;
    trace.events.push_back(event);

    E1Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect outputs > inputs";
}

// ============================================================================
// E2: Value Conservation Tests
// ============================================================================

TEST_F(EconomicSafetyTest, E2_NoViolation_ValueConserved) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with proper value conservation
    EconomicEvent event;
    event.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    event.timestamp = 200;
    event.node_id = "alice";
    event.tx_id = "tx1";
    event.fee_una = 5000;
    event.input_value = 105000;
    event.output_value = 100000;
    trace.events.push_back(event);

    E2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Value conservation should hold";
}

TEST_F(EconomicSafetyTest, E2_Violation_NegativeFee) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Confirmed transaction with negative fee
    EconomicEvent event;
    event.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    event.timestamp = 200;
    event.node_id = "alice";
    event.tx_id = "tx_bad";
    event.fee_una = 0;
    event.input_value = 100000;
    event.output_value = 105000;  // Violation!
    trace.events.push_back(event);

    E2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect negative fee";
    EXPECT_EQ(violations[0].property_name, "E2: Value Conservation");
}

TEST_F(EconomicSafetyTest, E2_Violation_FeeMismatch) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Confirmed transaction with fee mismatch
    EconomicEvent event;
    event.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    event.timestamp = 200;
    event.node_id = "alice";
    event.tx_id = "tx_mismatch";
    event.fee_una = 10000;  // Wrong!
    event.input_value = 105000;
    event.output_value = 100000;  // Actual fee: 5000
    trace.events.push_back(event);

    E2Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect fee mismatch";
}

// ============================================================================
// E3: Fee Overflow Protection Tests
// ============================================================================

TEST_F(EconomicSafetyTest, E3_NoViolation_ReasonableFee) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with reasonable fee
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 300;
    event.node_id = "alice";
    event.tx_id = "tx1";
    event.fee_una = 50000;  // 50k sats, reasonable
    event.success = true;
    trace.events.push_back(event);

    E3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Reasonable fee should not violate";
}

TEST_F(EconomicSafetyTest, E3_Violation_UnreasonablyHighFee) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with unreasonably high fee (> 1 BTC)
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 300;
    event.node_id = "alice";
    event.tx_id = "tx_overflow";
    event.fee_una = 200000000;  // 2 BTC, unreasonable!
    event.success = true;
    trace.events.push_back(event);

    E3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect unreasonably high fee";
    EXPECT_EQ(violations[0].property_name, "E3: Fee Overflow Protection");
}

TEST_F(EconomicSafetyTest, E3_Violation_ConfirmedUnreasonableFee) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Confirmed transaction with unreasonable fee
    EconomicEvent event;
    event.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    event.timestamp = 300;
    event.node_id = "alice";
    event.tx_id = "tx_overflow2";
    event.fee_una = 150000000;  // 1.5 BTC
    trace.events.push_back(event);

    E3Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect unreasonable confirmed fee";
}

// ============================================================================
// E4: Minimum Relay Fee Tests
// ============================================================================

TEST_F(EconomicSafetyTest, E4_NoViolation_AboveMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee above minimum
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 400;
    event.node_id = "alice";
    event.tx_id = "tx1";
    event.fee_una = 2000;  // Above minimum (1000)
    event.success = true;
    trace.events.push_back(event);

    E4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Fee above minimum should not violate";
}

TEST_F(EconomicSafetyTest, E4_Violation_BelowMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee below minimum
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 400;
    event.node_id = "alice";
    event.tx_id = "tx_lowfee";
    event.fee_una = 500;  // Below minimum (1000)
    event.success = true;
    trace.events.push_back(event);

    E4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect fee below minimum";
    EXPECT_EQ(violations[0].property_name, "E4: Minimum Relay Fee");
}

TEST_F(EconomicSafetyTest, E4_NoViolation_ExactlyMinimum) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with fee exactly at minimum
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 400;
    event.node_id = "alice";
    event.tx_id = "tx_exact";
    event.fee_una = 1000;  // Exactly minimum
    event.success = true;
    trace.events.push_back(event);

    E4Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Fee at minimum should be allowed";
}

// ============================================================================
// E5: Dust Threshold Tests
// ============================================================================

TEST_F(EconomicSafetyTest, E5_NoViolation_AboveDust) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with outputs above dust
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 500;
    event.node_id = "alice";
    event.tx_id = "tx1";
    event.output_value = 100000;  // Well above dust (546)
    event.success = true;
    trace.events.push_back(event);

    E5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Outputs above dust should not violate";
}

TEST_F(EconomicSafetyTest, E5_Violation_DustOutput) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with dust output
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 500;
    event.node_id = "alice";
    event.tx_id = "tx_dust";
    event.output_value = 500;  // Below dust threshold (546)
    event.success = true;
    trace.events.push_back(event);

    E5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect dust output";
    EXPECT_EQ(violations[0].property_name, "E5: Dust Threshold");
}

TEST_F(EconomicSafetyTest, E5_Violation_ConfirmedDust) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Confirmed transaction with dust
    EconomicEvent event;
    event.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    event.timestamp = 500;
    event.node_id = "alice";
    event.tx_id = "tx_dust2";
    event.output_value = 400;  // Below dust
    trace.events.push_back(event);

    E5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect confirmed dust";
}

TEST_F(EconomicSafetyTest, E5_NoViolation_ExactlyDust) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with outputs exactly at dust threshold
    EconomicEvent event;
    event.type = EconomicEventType::TX_ACCEPTED_TO_MEMPOOL;
    event.timestamp = 500;
    event.node_id = "alice";
    event.tx_id = "tx_exact";
    event.output_value = 546;  // Exactly dust threshold
    event.success = true;
    trace.events.push_back(event);

    E5Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Exactly at dust threshold should be allowed";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
