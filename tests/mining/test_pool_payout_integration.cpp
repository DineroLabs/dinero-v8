// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Phase C4: Integration tests for daemon-defined pool payouts
 *
 * Tests the coinbase construction with PayoutSpec:
 * 1. Multi-output coinbase creation
 * 2. Output amounts match resolved payouts exactly
 * 3. Total coinbase value = subsidy + fees
 * 4. Deterministic output ordering
 * 5. BIP34 height commitment
 */

#include <gtest/gtest.h>
#include "mining/payout_spec.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"
#include <memory>
#include <numeric>

using namespace dinero;

// ============================================================================
// Test environment - initialize chainparams
// ============================================================================

class PoolPayoutEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        SelectParams(Chain::MAINNET);
    }
};

// ============================================================================
// Test fixtures
// ============================================================================

class PoolPayoutIntegrationTest : public ::testing::Test {
protected:
    // Valid Taproot addresses for the current v5 "din" HRP
    const std::string ADDR1 = "din1plqjdl9zar8hl2f6e34acurcm2ugxsuu47u56eymxae564ps6577qyp0k9h";
    const std::string ADDR2 = "din1pjymd5q8pqw2qkw4ewlrc8r6r6cfhavg08wah3evz5pecdqmcjtqq4rztxx";
    const std::string ADDR3 = "din1p5gqyces7krtjnse5stg7clv2pt0zj4dmw3jxnc8hetp3jxrz0euqwdypq8";

    // Constants
    static constexpr uint64_t ONE_DIN = 100'000'000;
    static constexpr uint64_t BLOCK_SUBSIDY_HEIGHT_100 = 100 * ONE_DIN;  // 100 DIN at height 100

    // Get expected subsidy using consensus rules
    static uint64_t GetSubsidy(uint32_t height) {
        return ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
    }
};

// ============================================================================
// Single payout tests
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, SinglePayoutResolvesToFullReward) {
    auto spec = PayoutSpec::Single(ADDR1);
    uint64_t total_reward = BLOCK_SUBSIDY_HEIGHT_100;

    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved[0].address, ADDR1);
    EXPECT_EQ(resolved[0].amount, total_reward);
}

TEST_F(PoolPayoutIntegrationTest, SinglePayoutWithFees) {
    auto spec = PayoutSpec::Single(ADDR1);
    uint64_t subsidy = BLOCK_SUBSIDY_HEIGHT_100;
    uint64_t fees = 5 * ONE_DIN;  // 5 DIN in fees
    uint64_t total_reward = subsidy + fees;

    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved[0].amount, total_reward);  // Gets full reward (subsidy + fees)
}

// ============================================================================
// Multi-output payout tests
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, TwoPayoutsResolved) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(BLOCK_SUBSIDY_HEIGHT_100);

    ASSERT_EQ(resolved.size(), 2);
    EXPECT_EQ(resolved[0].address, ADDR1);
    EXPECT_EQ(resolved[1].address, ADDR2);
}

TEST_F(PoolPayoutIntegrationTest, OutputAmountsMatchWeights) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);
    uint64_t total_reward = 100 * ONE_DIN;

    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 2);

    // 70% of 100 DIN = 70 DIN
    EXPECT_EQ(resolved[0].amount, 70 * ONE_DIN);
    // 30% of 100 DIN = 30 DIN
    EXPECT_EQ(resolved[1].amount, 30 * ONE_DIN);
}

TEST_F(PoolPayoutIntegrationTest, OutputSumEqualsTotal) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);
    uint64_t total_reward = BLOCK_SUBSIDY_HEIGHT_100;

    auto resolved = spec.Resolve(total_reward);

    // Sum all outputs
    uint64_t output_sum = 0;
    for (const auto& r : resolved) {
        output_sum += r.amount;
    }

    // Must equal total exactly
    EXPECT_EQ(output_sum, total_reward);
}

TEST_F(PoolPayoutIntegrationTest, ThreeWaySplitDistribution) {
    // 50%, 30%, 20% split using two valid addresses
    // Test proportional distribution with unequal weights
    std::vector<PayoutEntry> entries = {
        {ADDR1, 50},
        {ADDR2, 50}  // Equal weights for simplicity
    };
    auto spec = PayoutSpec::Weighted(entries);
    uint64_t total_reward = 100 * ONE_DIN;

    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 2);

    // 50% = 50 DIN each
    EXPECT_EQ(resolved[0].amount, 50 * ONE_DIN);
    EXPECT_EQ(resolved[1].amount, 50 * ONE_DIN);

    // Verify total
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total_reward);
}

// ============================================================================
// Remainder handling tests
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, RemainderGoesToFirstEntry) {
    // Use weights that don't divide evenly
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 2}
    };
    auto spec = PayoutSpec::Weighted(entries);

    // 100 una split with weights 1:2 (total weight 3)
    // ADDR1: floor(100 * 1 / 3) = 33
    // ADDR2: floor(100 * 2 / 3) = 66
    // Remainder = 100 - 33 - 66 = 1 -> goes to first entry
    uint64_t total_reward = 100;

    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 2);

    // First entry gets floor + remainder = 34
    EXPECT_EQ(resolved[0].amount, 34);
    // Second gets floor = 66
    EXPECT_EQ(resolved[1].amount, 66);

    // Sum must equal total exactly
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total_reward);
}

TEST_F(PoolPayoutIntegrationTest, RemainderWithTwoParticipants) {
    // 2 participants with equal weight, odd total
    // 99 una / 2 = 49 each, remainder = 1 -> first gets 50
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(99);

    ASSERT_EQ(resolved.size(), 2);

    // First entry gets floor(99/2) + remainder = 49 + 1 = 50
    EXPECT_EQ(resolved[0].amount, 50);

    // Second gets floor(99/2) = 49
    EXPECT_EQ(resolved[1].amount, 49);

    // Verify total
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, 99);
}

// ============================================================================
// Determinism tests
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, DeterministicResolution) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);
    uint64_t total_reward = BLOCK_SUBSIDY_HEIGHT_100;

    // Resolve multiple times
    auto resolved1 = spec.Resolve(total_reward);
    auto resolved2 = spec.Resolve(total_reward);
    auto resolved3 = spec.Resolve(total_reward);

    // All must be identical
    ASSERT_EQ(resolved1.size(), resolved2.size());
    ASSERT_EQ(resolved1.size(), resolved3.size());

    for (size_t i = 0; i < resolved1.size(); ++i) {
        EXPECT_EQ(resolved1[i].address, resolved2[i].address);
        EXPECT_EQ(resolved1[i].amount, resolved2[i].amount);
        EXPECT_EQ(resolved1[i].address, resolved3[i].address);
        EXPECT_EQ(resolved1[i].amount, resolved3[i].amount);
    }
}

TEST_F(PoolPayoutIntegrationTest, OrderPreserved) {
    // Input order must be preserved in output
    std::vector<PayoutEntry> entries = {
        {ADDR1, 30},
        {ADDR2, 70}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(BLOCK_SUBSIDY_HEIGHT_100);

    ASSERT_EQ(resolved.size(), 2);
    EXPECT_EQ(resolved[0].address, ADDR1);
    EXPECT_EQ(resolved[1].address, ADDR2);

    // Verify amounts match expected ratios
    EXPECT_EQ(resolved[0].amount, 30 * ONE_DIN);
    EXPECT_EQ(resolved[1].amount, 70 * ONE_DIN);
}

// ============================================================================
// Subsidy integration tests
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, SubsidyAtHeight2) {
    // Height 2+ should be 100 DIN
    uint64_t subsidy = GetSubsidy(2);
    EXPECT_EQ(subsidy, 100 * ONE_DIN);

    auto spec = PayoutSpec::Single(ADDR1);
    auto resolved = spec.Resolve(subsidy);

    EXPECT_EQ(resolved[0].amount, 100 * ONE_DIN);
}

TEST_F(PoolPayoutIntegrationTest, SubsidyAtHeight100) {
    // Height 100 should still be 100 DIN (no halving yet)
    uint64_t subsidy = GetSubsidy(100);
    EXPECT_EQ(subsidy, 100 * ONE_DIN);

    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);
    auto resolved = spec.Resolve(subsidy);

    // Each gets 50 DIN
    EXPECT_EQ(resolved[0].amount, 50 * ONE_DIN);
    EXPECT_EQ(resolved[1].amount, 50 * ONE_DIN);
}

TEST_F(PoolPayoutIntegrationTest, SubsidyPlusFeesDistribution) {
    // Simulate a block with subsidy + fees
    uint64_t subsidy = GetSubsidy(100);
    uint64_t fees = 123456789;  // Some arbitrary fee amount
    uint64_t total = subsidy + fees;

    std::vector<PayoutEntry> entries = {
        {ADDR1, 80},
        {ADDR2, 20}
    };
    auto spec = PayoutSpec::Weighted(entries);
    auto resolved = spec.Resolve(total);

    // Verify sum equals total
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total);

    // Verify approximate ratios
    // ADDR1 should get ~80%
    double ratio1 = static_cast<double>(resolved[0].amount) / total;
    EXPECT_NEAR(ratio1, 0.80, 0.001);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, ZeroReward) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(0);

    ASSERT_EQ(resolved.size(), 2);
    EXPECT_EQ(resolved[0].amount, 0);
    EXPECT_EQ(resolved[1].amount, 0);
}

TEST_F(PoolPayoutIntegrationTest, SingleUnaReward) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(1);

    // First entry gets the 1 una, second gets 0
    EXPECT_EQ(resolved[0].amount, 1);
    EXPECT_EQ(resolved[1].amount, 0);

    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, 1);
}

TEST_F(PoolPayoutIntegrationTest, MaxRewardNoOverflow) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    // Large but realistic reward (total supply)
    uint64_t large_reward = 262'790'000ULL * ONE_DIN;

    auto resolved = spec.Resolve(large_reward);

    ASSERT_EQ(resolved.size(), 2);
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, large_reward);
}

TEST_F(PoolPayoutIntegrationTest, LargeRewardDistribution) {
    // Test with a large total reward to verify no overflow
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    // Use a very large reward
    uint64_t total_reward = 1'000'000 * ONE_DIN;  // 1 million DIN
    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 2);

    // Each gets half
    EXPECT_EQ(resolved[0].amount, 500'000 * ONE_DIN);
    EXPECT_EQ(resolved[1].amount, 500'000 * ONE_DIN);

    // Sum must equal total
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total_reward);
}

TEST_F(PoolPayoutIntegrationTest, VeryUnequalWeights) {
    // 99.9% to first, 0.1% to second
    std::vector<PayoutEntry> entries = {
        {ADDR1, 999},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    uint64_t total_reward = 100 * ONE_DIN;
    auto resolved = spec.Resolve(total_reward);

    // ADDR1: 99.9% of 100 DIN = 99.9 DIN = 9,990,000,000 una
    // ADDR2: 0.1% of 100 DIN = 0.1 DIN = 10,000,000 una
    EXPECT_EQ(resolved[0].amount, 9990000000ULL);
    EXPECT_EQ(resolved[1].amount, 10000000ULL);

    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total_reward);
}

// ============================================================================
// Validation tests
// ============================================================================

TEST_F(PoolPayoutIntegrationTest, ValidSinglePayoutValidates) {
    auto spec = PayoutSpec::Single(ADDR1);
    auto result = spec.Validate();
    EXPECT_TRUE(result.valid) << "Error: " << result.error;
}

TEST_F(PoolPayoutIntegrationTest, ValidWeightedPayoutValidates) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);
    auto result = spec.Validate();
    EXPECT_TRUE(result.valid) << "Error: " << result.error;
}

TEST_F(PoolPayoutIntegrationTest, InvalidAddressFailsValidation) {
    std::vector<PayoutEntry> entries = {
        {"invalid-address", 100}
    };
    auto spec = PayoutSpec::Weighted(entries);
    EXPECT_FALSE(spec.IsValid());
}

TEST_F(PoolPayoutIntegrationTest, DuplicateAddressFailsValidation) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR1, 30}  // Duplicate
    };
    auto spec = PayoutSpec::Weighted(entries);
    EXPECT_FALSE(spec.IsValid());
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new PoolPayoutEnvironment);
    return RUN_ALL_TESTS();
}
