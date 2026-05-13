#include <gtest/gtest.h>
#include "../properties/economic_attack_oracle_e16.h"
#include "../properties/economic_attack_oracle_e17.h"
#include "../properties/economic_attack_oracle_e18.h"
#include "../properties/economic_attack_oracle_e19.h"
#include "../properties/economic_attack_oracle_e20.h"

using namespace dinero::economic::test;

class EconomicAttackTest : public ::testing::Test {
protected:
    void SetUp() override {
        policy_ = EconomicPolicy();
        policy_.min_relay_fee_una = 1000;
        policy_.dust_threshold_una = 546;
    }

    EconomicPolicy policy_;
};

// ============================================================================
// E16: Double-Spend Attack Resistance Tests
// ============================================================================

TEST_F(EconomicAttackTest, E16_NoViolation_SingleConfirmation) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction confirmed once
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 200;
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx1";
    trace.events.push_back(confirmed);

    E16Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Single confirmation is valid";
}

TEST_F(EconomicAttackTest, E16_Violation_DoubleSpend) {
    EconomicTrace trace;
    trace.policy = policy_;

    // First version confirmed
    EconomicEvent confirmed1;
    confirmed1.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed1.timestamp = 200;
    confirmed1.node_id = "alice";
    confirmed1.tx_id = "tx_spend_v1";  // Conflicting tx (v1)
    confirmed1.block_height = 10;
    trace.events.push_back(confirmed1);

    // Second version also confirmed (double-spend!)
    EconomicEvent confirmed2;
    confirmed2.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed2.timestamp = 300;
    confirmed2.node_id = "alice";
    confirmed2.tx_id = "tx_spend_v2";  // Conflicting tx (v2)
    confirmed2.block_height = 11;
    trace.events.push_back(confirmed2);

    E16Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect double-spend";
    EXPECT_EQ(violations[0].property_name, "E16: Double-Spend Attack Resistance");
}

TEST_F(EconomicAttackTest, E16_NoViolation_NonConflicting) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Two different transactions (not conflicting)
    EconomicEvent confirmed1;
    confirmed1.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed1.timestamp = 200;
    confirmed1.node_id = "alice";
    confirmed1.tx_id = "tx1";
    trace.events.push_back(confirmed1);

    EconomicEvent confirmed2;
    confirmed2.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed2.timestamp = 300;
    confirmed2.node_id = "alice";
    confirmed2.tx_id = "tx2";
    trace.events.push_back(confirmed2);

    E16Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Different transactions are not double-spend";
}

// ============================================================================
// E17: Fee Sniping Resistance Tests
// ============================================================================

TEST_F(EconomicAttackTest, E17_NoViolation_NoReorg) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction confirmed once, no reorg
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 200;
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx1";
    trace.events.push_back(confirmed);

    E17Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "No reorg is valid";
}

TEST_F(EconomicAttackTest, E17_Violation_FeeSniping) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction confirmed
    EconomicEvent confirmed1;
    confirmed1.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed1.timestamp = 200;
    confirmed1.node_id = "alice";
    confirmed1.tx_id = "tx_highfee";
    trace.events.push_back(confirmed1);

    // Reorged out
    EconomicEvent reorged;
    reorged.type = EconomicEventType::TX_REORGED_OUT;
    reorged.timestamp = 300;
    reorged.node_id = "alice";
    reorged.tx_id = "tx_highfee";
    trace.events.push_back(reorged);

    // Re-confirmed (fee sniping!)
    EconomicEvent confirmed2;
    confirmed2.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed2.timestamp = 400;
    confirmed2.node_id = "alice";
    confirmed2.tx_id = "tx_highfee";
    trace.events.push_back(confirmed2);

    E17Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect fee sniping pattern";
    EXPECT_EQ(violations[0].property_name, "E17: Fee Sniping Resistance");
}

// ============================================================================
// E18: Transaction Malleability Resistance Tests
// ============================================================================

TEST_F(EconomicAttackTest, E18_NoViolation_StableID) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with stable ID
    EconomicEvent confirmed;
    confirmed.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    confirmed.timestamp = 200;
    confirmed.node_id = "alice";
    confirmed.tx_id = "tx1";
    trace.events.push_back(confirmed);

    E18Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Stable ID is valid";
}

TEST_F(EconomicAttackTest, E18_Violation_Malleability) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with malleated ID
    EconomicEvent malleated;
    malleated.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    malleated.timestamp = 200;
    malleated.node_id = "alice";
    malleated.tx_id = "tx1_malleated";  // Indicates malleability
    trace.events.push_back(malleated);

    E18Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect malleability";
    EXPECT_EQ(violations[0].property_name, "E18: Transaction Malleability Resistance");
}

TEST_F(EconomicAttackTest, E18_Violation_MalSuffix) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Transaction with "_mal" suffix
    EconomicEvent malleated;
    malleated.type = EconomicEventType::TX_INCLUDED_IN_BLOCK;
    malleated.timestamp = 200;
    malleated.node_id = "alice";
    malleated.tx_id = "tx1_mal";  // "_mal" indicates malleability
    trace.events.push_back(malleated);

    E18Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect malleability from _mal suffix";
}

// ============================================================================
// E19: Time-Warp Attack Resistance Tests
// ============================================================================

TEST_F(EconomicAttackTest, E19_NoViolation_MonotonicTime) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Blocks with monotonically increasing timestamps
    EconomicEvent block1;
    block1.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block1.timestamp = 100;
    block1.node_id = "alice";
    trace.events.push_back(block1);

    EconomicEvent block2;
    block2.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block2.timestamp = 200;
    block2.node_id = "alice";
    trace.events.push_back(block2);

    EconomicEvent block3;
    block3.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block3.timestamp = 300;
    block3.node_id = "alice";
    trace.events.push_back(block3);

    E19Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Monotonic timestamps are valid";
}

TEST_F(EconomicAttackTest, E19_Violation_TimeWarp) {
    EconomicTrace trace;
    trace.policy = policy_;

    // First block
    EconomicEvent block1;
    block1.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block1.timestamp = 200;
    block1.node_id = "alice";
    trace.events.push_back(block1);

    // Second block with earlier timestamp (time warp!)
    EconomicEvent block2;
    block2.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block2.timestamp = 100;  // Goes backwards!
    block2.node_id = "alice";
    trace.events.push_back(block2);

    E19Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect time warp";
    EXPECT_EQ(violations[0].property_name, "E19: Time-Warp Attack Resistance");
}

// ============================================================================
// E20: Selfish Mining Resistance Tests
// ============================================================================

TEST_F(EconomicAttackTest, E20_NoViolation_RegularMining) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Blocks with normal spacing
    EconomicEvent block1;
    block1.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block1.timestamp = 100;
    block1.node_id = "alice";
    trace.events.push_back(block1);

    EconomicEvent block2;
    block2.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block2.timestamp = 200;  // 100 time units later
    block2.node_id = "alice";
    trace.events.push_back(block2);

    E20Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_TRUE(violations.empty()) << "Regular mining is valid";
}

TEST_F(EconomicAttackTest, E20_Violation_SelfishMining) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Multiple blocks released very close together (batch release)
    EconomicEvent block1;
    block1.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block1.timestamp = 100;
    block1.node_id = "alice";
    trace.events.push_back(block1);

    EconomicEvent block2;
    block2.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block2.timestamp = 102;  // Very close (within threshold)
    block2.node_id = "alice";
    trace.events.push_back(block2);

    E20Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect selfish mining pattern";
    EXPECT_EQ(violations[0].property_name, "E20: Selfish Mining Resistance");
}

TEST_F(EconomicAttackTest, E20_Violation_MultipleBatchRelease) {
    EconomicTrace trace;
    trace.policy = policy_;

    // Three blocks released very close together
    EconomicEvent block1;
    block1.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block1.timestamp = 100;
    block1.node_id = "alice";
    trace.events.push_back(block1);

    EconomicEvent block2;
    block2.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block2.timestamp = 103;
    block2.node_id = "alice";
    trace.events.push_back(block2);

    EconomicEvent block3;
    block3.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    block3.timestamp = 106;
    block3.node_id = "alice";
    trace.events.push_back(block3);

    E20Oracle oracle;
    auto violations = oracle.check(trace);

    EXPECT_FALSE(violations.empty()) << "Should detect multiple batch release";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
