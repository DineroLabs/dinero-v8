/**
 * @file test_block_height.cpp
 * @brief Phase M.6.0: Comprehensive unit tests for BlockHeight type
 *
 * Tests coverage:
 * - Construction (domain-locked, special heights)
 * - Overflow protection (Add, GetSpendableHeight)
 * - Underflow protection (Sub, Prev)
 * - Maturity calculations (IsMatureAt, BlocksUntilMature)
 * - Boundary conversions (AsInt, FromInt, AsUint32, FromUint32)
 * - Comparison operators
 * - Null/invalid height handling
 */

#include <gtest/gtest.h>
#include "primitives/block_height.h"

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════
// Construction Tests
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, DefaultConstructor) {
    BlockHeight height;
    EXPECT_TRUE(height.IsNull());
    EXPECT_FALSE(height.IsValid());
}

TEST(BlockHeight, GenesisConstructor) {
    BlockHeight height = BlockHeight::Genesis();
    EXPECT_TRUE(height.IsGenesis());
    EXPECT_TRUE(height.IsValid());
    EXPECT_EQ(height.AsUint32(), 0U);
}

TEST(BlockHeight, FirstPoWConstructor) {
    BlockHeight height = BlockHeight::FirstPoW();
    EXPECT_TRUE(height.IsPoW());
    EXPECT_TRUE(height.IsValid());
    EXPECT_EQ(height.AsUint32(), 1U);
}

TEST(BlockHeight, FromUint32Valid) {
    auto height = BlockHeight::FromUint32(100);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->AsUint32(), 100U);
    EXPECT_TRUE(height->IsValid());
}

TEST(BlockHeight, FromUint32Invalid) {
    uint32_t too_large = UINT32_MAX;
    auto height = BlockHeight::FromUint32(too_large);
    EXPECT_FALSE(height.has_value());  // Exceeds MAX_HEIGHT
}

TEST(BlockHeight, FromIntValid) {
    auto height = BlockHeight::FromInt(50);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->AsUint32(), 50U);
}

TEST(BlockHeight, FromIntNegative) {
    auto height = BlockHeight::FromInt(-1);
    EXPECT_FALSE(height.has_value());  // Negative heights rejected
}

// ═══════════════════════════════════════════════════════════════════
// Validation Tests
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, IsGenesis) {
    EXPECT_TRUE(BlockHeight::Genesis().IsGenesis());
    EXPECT_FALSE(BlockHeight::FirstPoW().IsGenesis());
}

TEST(BlockHeight, IsPoW) {
    EXPECT_FALSE(BlockHeight::Genesis().IsPoW());
    EXPECT_TRUE(BlockHeight::FirstPoW().IsPoW());

    auto height100 = BlockHeight::FromUint32(100);
    ASSERT_TRUE(height100.has_value());
    EXPECT_TRUE(height100->IsPoW());
}

TEST(BlockHeight, IsNull) {
    BlockHeight null_height;
    EXPECT_TRUE(null_height.IsNull());

    BlockHeight valid = BlockHeight::Genesis();
    EXPECT_FALSE(valid.IsNull());
}

TEST(BlockHeight, IsValid) {
    BlockHeight null_height;
    EXPECT_FALSE(null_height.IsValid());

    BlockHeight genesis = BlockHeight::Genesis();
    EXPECT_TRUE(genesis.IsValid());

    auto valid = BlockHeight::FromUint32(1000000);
    ASSERT_TRUE(valid.has_value());
    EXPECT_TRUE(valid->IsValid());
}

// ═══════════════════════════════════════════════════════════════════
// Maturity Tests (Coinbase Logic)
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, IsMatureAtExact) {
    auto coinbase = BlockHeight::FromUint32(10);
    auto current = BlockHeight::FromUint32(110);  // Exactly 100 blocks on top

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_TRUE(coinbase->IsMatureAt(*current));
}

TEST(BlockHeight, IsMatureAtJustMature) {
    auto coinbase = BlockHeight::FromUint32(10);
    auto current = BlockHeight::FromUint32(111);  // 101 blocks on top

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_TRUE(coinbase->IsMatureAt(*current));
}

TEST(BlockHeight, IsMatureAtImmature) {
    auto coinbase = BlockHeight::FromUint32(10);
    auto current = BlockHeight::FromUint32(109);  // Only 99 blocks on top

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_FALSE(coinbase->IsMatureAt(*current));
}

TEST(BlockHeight, IsMatureAtCurrentBeforeCoinbase) {
    auto coinbase = BlockHeight::FromUint32(100);
    auto current = BlockHeight::FromUint32(50);  // Before coinbase

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_FALSE(coinbase->IsMatureAt(*current));
}

TEST(BlockHeight, GetSpendableHeightNormal) {
    auto coinbase = BlockHeight::FromUint32(10);
    ASSERT_TRUE(coinbase.has_value());

    auto spendable = coinbase->GetSpendableHeight();
    ASSERT_TRUE(spendable.has_value());
    EXPECT_EQ(spendable->AsUint32(), 110U);  // 10 + 100
}

TEST(BlockHeight, GetSpendableHeightGenesis) {
    BlockHeight coinbase = BlockHeight::Genesis();

    auto spendable = coinbase.GetSpendableHeight();
    ASSERT_TRUE(spendable.has_value());
    EXPECT_EQ(spendable->AsUint32(), 100U);  // 0 + 100
}

TEST(BlockHeight, GetSpendableHeightOverflow) {
    // Use MAX_HEIGHT - 50, which is valid but overflows when adding COINBASE_MATURITY (100)
    uint32_t max_height = HeightConstants::MAX_HEIGHT;
    auto coinbase = BlockHeight::FromUint32(max_height - 50);
    ASSERT_TRUE(coinbase.has_value());

    auto spendable = coinbase->GetSpendableHeight();
    EXPECT_FALSE(spendable.has_value());  // Would overflow past MAX_HEIGHT
}

TEST(BlockHeight, BlocksUntilMatureAlreadyMature) {
    auto coinbase = BlockHeight::FromUint32(10);
    auto current = BlockHeight::FromUint32(200);

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_EQ(coinbase->BlocksUntilMature(*current), 0U);
}

TEST(BlockHeight, BlocksUntilMaturePartial) {
    auto coinbase = BlockHeight::FromUint32(10);
    auto current = BlockHeight::FromUint32(80);  // 70 blocks on top

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_EQ(coinbase->BlocksUntilMature(*current), 30U);  // 100 - 70
}

TEST(BlockHeight, BlocksUntilMatureZeroBlocks) {
    auto coinbase = BlockHeight::FromUint32(10);
    auto current = BlockHeight::FromUint32(10);  // Same block

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    EXPECT_EQ(coinbase->BlocksUntilMature(*current), 100U);  // Full maturity needed
}

TEST(BlockHeight, BlocksUntilMatureCurrentBefore) {
    auto coinbase = BlockHeight::FromUint32(100);
    auto current = BlockHeight::FromUint32(50);  // Before coinbase

    ASSERT_TRUE(coinbase.has_value());
    ASSERT_TRUE(current.has_value());

    // Conservative: returns full COINBASE_MATURITY
    EXPECT_EQ(coinbase->BlocksUntilMature(*current), 100U);
}

// ═══════════════════════════════════════════════════════════════════
// Arithmetic Tests (Overflow-Safe)
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, NextNormal) {
    auto height = BlockHeight::FromUint32(10);
    ASSERT_TRUE(height.has_value());

    auto next = height->Next();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->AsUint32(), 11U);
}

TEST(BlockHeight, NextGenesis) {
    BlockHeight genesis = BlockHeight::Genesis();

    auto next = genesis.Next();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->AsUint32(), 1U);
    EXPECT_TRUE(next->IsPoW());
}

TEST(BlockHeight, NextOverflow) {
    // Use MAX_HEIGHT, which is valid but overflows when incremented
    auto height = BlockHeight::FromUint32(HeightConstants::MAX_HEIGHT);
    ASSERT_TRUE(height.has_value());

    auto next = height->Next();
    EXPECT_FALSE(next.has_value());  // Would overflow past UINT32_MAX
}

TEST(BlockHeight, PrevNormal) {
    auto height = BlockHeight::FromUint32(10);
    ASSERT_TRUE(height.has_value());

    auto prev = height->Prev();
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->AsUint32(), 9U);
}

TEST(BlockHeight, PrevFirstPoW) {
    BlockHeight first_pow = BlockHeight::FirstPoW();

    auto prev = first_pow.Prev();
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(prev->AsUint32(), 0U);
    EXPECT_TRUE(prev->IsGenesis());
}

TEST(BlockHeight, PrevGenesisUnderflow) {
    BlockHeight genesis = BlockHeight::Genesis();

    auto prev = genesis.Prev();
    EXPECT_FALSE(prev.has_value());  // Cannot go below genesis
}

TEST(BlockHeight, AddNormal) {
    auto height = BlockHeight::FromUint32(10);
    ASSERT_TRUE(height.has_value());

    auto result = height->Add(50);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->AsUint32(), 60U);
}

TEST(BlockHeight, AddZero) {
    auto height = BlockHeight::FromUint32(10);
    ASSERT_TRUE(height.has_value());

    auto result = height->Add(0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->AsUint32(), 10U);
}

TEST(BlockHeight, AddOverflow) {
    // Use MAX_HEIGHT - 10, which is valid but overflows when adding 20
    auto height = BlockHeight::FromUint32(HeightConstants::MAX_HEIGHT - 10);
    ASSERT_TRUE(height.has_value());

    auto result = height->Add(20);
    EXPECT_FALSE(result.has_value());  // Would overflow past UINT32_MAX
}

TEST(BlockHeight, SubNormal) {
    auto height = BlockHeight::FromUint32(100);
    ASSERT_TRUE(height.has_value());

    auto result = height->Sub(30);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->AsUint32(), 70U);
}

TEST(BlockHeight, SubExact) {
    auto height = BlockHeight::FromUint32(50);
    ASSERT_TRUE(height.has_value());

    auto result = height->Sub(50);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->AsUint32(), 0U);
    EXPECT_TRUE(result->IsGenesis());
}

TEST(BlockHeight, SubUnderflow) {
    auto height = BlockHeight::FromUint32(10);
    ASSERT_TRUE(height.has_value());

    auto result = height->Sub(20);
    EXPECT_FALSE(result.has_value());  // Would underflow
}

TEST(BlockHeight, DistanceToSame) {
    auto height1 = BlockHeight::FromUint32(100);
    auto height2 = BlockHeight::FromUint32(100);

    ASSERT_TRUE(height1.has_value());
    ASSERT_TRUE(height2.has_value());

    auto distance = height1->DistanceTo(*height2);
    ASSERT_TRUE(distance.has_value());
    EXPECT_EQ(*distance, 0U);
}

TEST(BlockHeight, DistanceToForward) {
    auto height1 = BlockHeight::FromUint32(10);
    auto height2 = BlockHeight::FromUint32(50);

    ASSERT_TRUE(height1.has_value());
    ASSERT_TRUE(height2.has_value());

    auto distance = height1->DistanceTo(*height2);
    ASSERT_TRUE(distance.has_value());
    EXPECT_EQ(*distance, 40U);
}

TEST(BlockHeight, DistanceToBackward) {
    auto height1 = BlockHeight::FromUint32(50);
    auto height2 = BlockHeight::FromUint32(10);

    ASSERT_TRUE(height1.has_value());
    ASSERT_TRUE(height2.has_value());

    auto distance = height1->DistanceTo(*height2);
    ASSERT_TRUE(distance.has_value());
    EXPECT_EQ(*distance, 40U);  // Absolute distance
}

TEST(BlockHeight, DistanceToInvalid) {
    BlockHeight null_height;
    auto valid = BlockHeight::FromUint32(10);

    ASSERT_TRUE(valid.has_value());

    auto distance = null_height.DistanceTo(*valid);
    EXPECT_FALSE(distance.has_value());  // Null height is invalid
}

// ═══════════════════════════════════════════════════════════════════
// Boundary Conversion Tests
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, AsUint32) {
    auto height = BlockHeight::FromUint32(12345);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->AsUint32(), 12345U);
}

TEST(BlockHeight, AsIntNormal) {
    auto height = BlockHeight::FromUint32(1000);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->AsInt(), 1000);
}

TEST(BlockHeight, AsIntMaxSafe) {
    auto height = BlockHeight::FromUint32(INT32_MAX);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->AsInt(), INT32_MAX);
}

TEST(BlockHeight, AsIntTooLarge) {
    auto height = BlockHeight::FromUint32(static_cast<uint32_t>(INT32_MAX) + 1);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->AsInt(), -1);  // Sentinel value
}

// ═══════════════════════════════════════════════════════════════════
// Comparison Operator Tests
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, EqualityOperators) {
    auto a = BlockHeight::FromUint32(100);
    auto b = BlockHeight::FromUint32(100);
    auto c = BlockHeight::FromUint32(200);

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());

    EXPECT_TRUE(*a == *b);
    EXPECT_FALSE(*a == *c);
    EXPECT_FALSE(*a != *b);
    EXPECT_TRUE(*a != *c);
}

TEST(BlockHeight, ComparisonOperators) {
    auto small = BlockHeight::FromUint32(10);
    auto large = BlockHeight::FromUint32(100);

    ASSERT_TRUE(small.has_value());
    ASSERT_TRUE(large.has_value());

    EXPECT_TRUE(*small < *large);
    EXPECT_TRUE(*small <= *large);
    EXPECT_FALSE(*small > *large);
    EXPECT_FALSE(*small >= *large);

    EXPECT_TRUE(*large > *small);
    EXPECT_TRUE(*large >= *small);
    EXPECT_FALSE(*large < *small);
    EXPECT_FALSE(*large <= *small);
}

TEST(BlockHeight, ComparisonEqual) {
    auto a = BlockHeight::FromUint32(50);
    auto b = BlockHeight::FromUint32(50);

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    EXPECT_TRUE(*a <= *b);
    EXPECT_TRUE(*a >= *b);
}

// ═══════════════════════════════════════════════════════════════════
// String Conversion Tests
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, ToStringNull) {
    BlockHeight null_height;
    EXPECT_EQ(null_height.ToString(), "null");
}

TEST(BlockHeight, ToStringGenesis) {
    BlockHeight genesis = BlockHeight::Genesis();
    EXPECT_EQ(genesis.ToString(), "0 (genesis)");
}

TEST(BlockHeight, ToStringFirstPoW) {
    BlockHeight first_pow = BlockHeight::FirstPoW();
    EXPECT_EQ(first_pow.ToString(), "1");
}

TEST(BlockHeight, ToStringNormal) {
    auto height = BlockHeight::FromUint32(12345);
    ASSERT_TRUE(height.has_value());
    EXPECT_EQ(height->ToString(), "12345");
}

// ═══════════════════════════════════════════════════════════════════
// Compile-Time Invariants
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, TypeInvariants) {
    // Size check
    EXPECT_EQ(sizeof(BlockHeight), 4);

    // Trivially copyable check
    EXPECT_TRUE(std::is_trivially_copyable<BlockHeight>::value);

    // No implicit conversions
    EXPECT_FALSE((std::is_convertible<uint32_t, BlockHeight>::value));
    EXPECT_FALSE((std::is_convertible<int, BlockHeight>::value));
    EXPECT_FALSE((std::is_convertible<BlockHeight, uint32_t>::value));
    EXPECT_FALSE((std::is_convertible<BlockHeight, int>::value));
}

// ═══════════════════════════════════════════════════════════════════
// Hash Function Tests
// ═══════════════════════════════════════════════════════════════════

TEST(BlockHeight, HashFunction) {
    auto a = BlockHeight::FromUint32(100);
    auto b = BlockHeight::FromUint32(100);
    auto c = BlockHeight::FromUint32(200);

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());

    std::hash<BlockHeight> hasher;

    // Equal heights should have equal hashes
    EXPECT_EQ(hasher(*a), hasher(*b));

    // Different heights (usually) have different hashes
    EXPECT_NE(hasher(*a), hasher(*c));
}

TEST(BlockHeight, HashInUnorderedMap) {
    std::unordered_map<BlockHeight, std::string> map;

    auto key1 = BlockHeight::FromUint32(10);
    auto key2 = BlockHeight::FromUint32(20);

    ASSERT_TRUE(key1.has_value());
    ASSERT_TRUE(key2.has_value());

    map[*key1] = "ten";
    map[*key2] = "twenty";

    EXPECT_EQ(map[*key1], "ten");
    EXPECT_EQ(map[*key2], "twenty");
    EXPECT_EQ(map.size(), 2);
}

// ═══════════════════════════════════════════════════════════════════
// Test Runner
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
