// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Tests for daemon-defined pool payouts (Phase C1)
 *
 * Tests:
 * 1. PayoutSpec construction and validation
 * 2. Weight normalization and amount calculation
 * 3. Deterministic remainder handling
 * 4. Edge cases (single payout, zero reward, max entries)
 * 5. JSON serialization round-trip
 */

#include <gtest/gtest.h>
#include "mining/payout_spec.h"
#include "consensus/chainparams.h"

using namespace dinero;

// Initialize chainparams before tests
class PayoutSpecEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Select mainnet params (uses "din" HRP)
        SelectParams(Chain::MAINNET);
    }
};

// ============================================================================
// Test fixtures and helpers
// ============================================================================

class PayoutSpecTest : public ::testing::Test {
protected:
    // Valid Taproot (P2TR) test addresses with current v5 "din" HRP checksums
    const std::string ADDR1 = "din1plqjdl9zar8hl2f6e34acurcm2ugxsuu47u56eymxae564ps6577qyp0k9h";
    const std::string ADDR2 = "din1pjymd5q8pqw2qkw4ewlrc8r6r6cfhavg08wah3evz5pecdqmcjtqq4rztxx";
    const std::string ADDR3 = "din1p5gqyces7krtjnse5stg7clv2pt0zj4dmw3jxnc8hetp3jxrz0euqwdypq8";

    // DIN amounts in una (1 DIN = 100,000,000 una)
    static constexpr uint64_t ONE_DIN = 100'000'000;
    static constexpr uint64_t BLOCK_REWARD = 50 * ONE_DIN;  // 50 DIN
};

// ============================================================================
// Construction tests
// ============================================================================

TEST_F(PayoutSpecTest, SinglePayoutConstruction) {
    auto spec = PayoutSpec::Single(ADDR1);

    EXPECT_TRUE(spec.IsSinglePayout());
    EXPECT_EQ(spec.Count(), 1);
    EXPECT_FALSE(spec.Empty());

    const auto& entries = spec.Entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].address, ADDR1);
    EXPECT_GT(entries[0].weight, 0);
}

TEST_F(PayoutSpecTest, WeightedPayoutConstruction) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_FALSE(spec.IsSinglePayout());
    EXPECT_EQ(spec.Count(), 2);

    const auto& result = spec.Entries();
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].address, ADDR1);
    EXPECT_EQ(result[0].weight, 70);
    EXPECT_EQ(result[1].address, ADDR2);
    EXPECT_EQ(result[1].weight, 30);
}

TEST_F(PayoutSpecTest, EmptySpecInvalid) {
    PayoutSpec spec;
    EXPECT_TRUE(spec.Empty());
    EXPECT_FALSE(spec.IsValid());

    auto result = spec.Validate();
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error.empty());
}

// ============================================================================
// Validation tests
// ============================================================================

TEST_F(PayoutSpecTest, ValidSinglePayout) {
    auto spec = PayoutSpec::Single(ADDR1);
    auto result = spec.Validate();

    EXPECT_TRUE(result.valid) << "Error: " << result.error;
}

TEST_F(PayoutSpecTest, ValidWeightedPayout) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);
    auto result = spec.Validate();

    EXPECT_TRUE(result.valid) << "Error: " << result.error;
}

TEST_F(PayoutSpecTest, ZeroWeightInvalid) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 0}  // Invalid: zero weight
    };
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_FALSE(spec.IsValid());
    auto result = spec.Validate();
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.error.find("zero weight") != std::string::npos);
}

TEST_F(PayoutSpecTest, EmptyAddressInvalid) {
    std::vector<PayoutEntry> entries = {
        {"", 50}  // Invalid: empty address
    };
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_FALSE(spec.IsValid());
}

TEST_F(PayoutSpecTest, DuplicateAddressInvalid) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 50},
        {ADDR1, 30}  // Invalid: duplicate
    };
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_FALSE(spec.IsValid());
    auto result = spec.Validate();
    EXPECT_TRUE(result.error.find("Duplicate") != std::string::npos);
}

TEST_F(PayoutSpecTest, TooManyEntriesInvalid) {
    std::vector<PayoutEntry> entries;
    // Add more than MAX_PAYOUT_ENTRIES
    for (uint32_t i = 0; i <= PayoutSpec::MAX_PAYOUT_ENTRIES; ++i) {
        // Create unique "addresses" (won't validate as bech32 but tests count limit)
        entries.emplace_back("addr" + std::to_string(i), 10);
    }
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_FALSE(spec.IsValid());
    auto result = spec.Validate();
    EXPECT_TRUE(result.error.find("maximum entries") != std::string::npos);
}

// ============================================================================
// Weight normalization and amount calculation tests
// ============================================================================

TEST_F(PayoutSpecTest, SinglePayoutResolution) {
    auto spec = PayoutSpec::Single(ADDR1);
    auto resolved = spec.Resolve(BLOCK_REWARD);

    ASSERT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved[0].address, ADDR1);
    EXPECT_EQ(resolved[0].amount, BLOCK_REWARD);  // Gets full reward
}

TEST_F(PayoutSpecTest, EqualWeightsSplitEvenly) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 50},
        {ADDR2, 50}
    };
    auto spec = PayoutSpec::Weighted(entries);

    uint64_t total_reward = 100 * ONE_DIN;  // 100 DIN, easily divisible
    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 2);
    // Each should get exactly 50 DIN
    EXPECT_EQ(resolved[0].amount, 50 * ONE_DIN);
    EXPECT_EQ(resolved[1].amount, 50 * ONE_DIN);

    // Sum must equal total
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total_reward);
}

TEST_F(PayoutSpecTest, UnequalWeightsSplitProportionally) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);

    uint64_t total_reward = 100 * ONE_DIN;
    auto resolved = spec.Resolve(total_reward);

    ASSERT_EQ(resolved.size(), 2);
    // First: 70% = 70 DIN
    EXPECT_EQ(resolved[0].amount, 70 * ONE_DIN);
    // Second: 30% = 30 DIN
    EXPECT_EQ(resolved[1].amount, 30 * ONE_DIN);

    // Sum must equal total
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, total_reward);
}

TEST_F(PayoutSpecTest, RemainderAssignedToFirstEntry) {
    // Test case where division doesn't work out evenly
    // Use 2 addresses with weights that don't divide evenly
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

TEST_F(PayoutSpecTest, DeterministicResolution) {
    // Same inputs must always produce same outputs
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);

    // Resolve multiple times
    auto resolved1 = spec.Resolve(BLOCK_REWARD);
    auto resolved2 = spec.Resolve(BLOCK_REWARD);
    auto resolved3 = spec.Resolve(BLOCK_REWARD);

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

TEST_F(PayoutSpecTest, ZeroRewardResolution) {
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

TEST_F(PayoutSpecTest, LargeRewardNoOverflow) {
    // Test with maximum possible reward (should not overflow)
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    // Large but realistic reward
    uint64_t large_reward = 21'000'000ULL * ONE_DIN;  // 21M DIN (total supply)
    auto resolved = spec.Resolve(large_reward);

    ASSERT_EQ(resolved.size(), 2);
    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, large_reward);
}

TEST_F(PayoutSpecTest, TotalWeightCalculation) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_EQ(spec.TotalWeight(), 100);
}

// ============================================================================
// JSON serialization tests
// ============================================================================

TEST_F(PayoutSpecTest, ToJsonSinglePayout) {
    auto spec = PayoutSpec::Single(ADDR1);
    std::string json = spec.ToJson();

    EXPECT_FALSE(json.empty());
    EXPECT_TRUE(json.find(ADDR1) != std::string::npos);
    EXPECT_TRUE(json.find("weight") != std::string::npos);
}

TEST_F(PayoutSpecTest, ToJsonWeightedPayout) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto spec = PayoutSpec::Weighted(entries);
    std::string json = spec.ToJson();

    EXPECT_TRUE(json.find(ADDR1) != std::string::npos);
    EXPECT_TRUE(json.find(ADDR2) != std::string::npos);
    EXPECT_TRUE(json.find("70") != std::string::npos);
    EXPECT_TRUE(json.find("30") != std::string::npos);
}

TEST_F(PayoutSpecTest, FromJsonRoundTrip) {
    std::vector<PayoutEntry> entries = {
        {ADDR1, 70},
        {ADDR2, 30}
    };
    auto original = PayoutSpec::Weighted(entries);

    // Serialize
    std::string json = original.ToJson();

    // Deserialize
    auto parsed = PayoutSpec::FromJson(json);
    ASSERT_TRUE(parsed.has_value());

    // Verify
    EXPECT_EQ(parsed->Count(), original.Count());
    const auto& orig_entries = original.Entries();
    const auto& parsed_entries = parsed->Entries();

    for (size_t i = 0; i < orig_entries.size(); ++i) {
        EXPECT_EQ(parsed_entries[i].address, orig_entries[i].address);
        EXPECT_EQ(parsed_entries[i].weight, orig_entries[i].weight);
    }
}

TEST_F(PayoutSpecTest, FromJsonInvalid) {
    // Invalid JSON
    auto result1 = PayoutSpec::FromJson("not json");
    EXPECT_FALSE(result1.has_value());

    // Not an array
    auto result2 = PayoutSpec::FromJson("{\"address\": \"test\"}");
    EXPECT_FALSE(result2.has_value());

    // Missing fields
    auto result3 = PayoutSpec::FromJson("[{\"address\": \"test\"}]");
    EXPECT_FALSE(result3.has_value());

    // Wrong types
    auto result4 = PayoutSpec::FromJson("[{\"address\": 123, \"weight\": \"abc\"}]");
    EXPECT_FALSE(result4.has_value());
}

// ============================================================================
// Edge case tests
// ============================================================================

TEST_F(PayoutSpecTest, SingleUnaRemainder) {
    // Edge case: 1 una total, 2 recipients
    std::vector<PayoutEntry> entries = {
        {ADDR1, 1},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(1);

    // First gets the 1 una, second gets 0
    // floor(1 * 1 / 2) = 0 for each, remainder 1 goes to first
    EXPECT_EQ(resolved[0].amount, 1);
    EXPECT_EQ(resolved[1].amount, 0);

    uint64_t sum = resolved[0].amount + resolved[1].amount;
    EXPECT_EQ(sum, 1);
}

TEST_F(PayoutSpecTest, VeryUnequalWeights) {
    // 99% to first, 1% to second
    std::vector<PayoutEntry> entries = {
        {ADDR1, 99},
        {ADDR2, 1}
    };
    auto spec = PayoutSpec::Weighted(entries);

    auto resolved = spec.Resolve(100 * ONE_DIN);

    // 99% of 100 DIN = 99 DIN
    EXPECT_EQ(resolved[0].amount, 99 * ONE_DIN);
    // 1% of 100 DIN = 1 DIN
    EXPECT_EQ(resolved[1].amount, 1 * ONE_DIN);
}

TEST_F(PayoutSpecTest, ManySmallWeights) {
    // 20 entries with equal weights
    std::vector<PayoutEntry> entries;
    for (int i = 0; i < 20; ++i) {
        // Create placeholder addresses (validation will fail but tests allocation logic)
        entries.emplace_back("addr" + std::to_string(i), 5);
    }
    auto spec = PayoutSpec::Weighted(entries);

    // Don't validate addresses in this test, just check count limit
    EXPECT_EQ(spec.Count(), 20);
    EXPECT_EQ(spec.TotalWeight(), 100);
}

// ============================================================================
// Address validation tests (integration with bech32)
// ============================================================================

TEST_F(PayoutSpecTest, InvalidAddressFormat) {
    // Test that invalid addresses fail validation
    std::vector<PayoutEntry> entries = {
        {"invalid-address", 50}  // Not bech32
    };
    auto spec = PayoutSpec::Weighted(entries);

    EXPECT_FALSE(spec.IsValid());
    auto result = spec.Validate();
    EXPECT_TRUE(result.error.find("invalid address") != std::string::npos);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    // Register environment to initialize chainparams
    ::testing::AddGlobalTestEnvironment(new PayoutSpecEnvironment);
    return RUN_ALL_TESTS();
}
