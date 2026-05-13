/**
 * @file test_amount.cpp
 * @brief Phase M.6.0: Comprehensive unit tests for AmountUna type
 *
 * Tests coverage:
 * - Construction (domain-locked, no implicit conversions)
 * - Overflow protection (Add, Mul)
 * - Underflow protection (Sub)
 * - Supply validation (IsWithinSupply)
 * - Boundary conversions (GetInt64, GetUna)
 * - Comparison operators
 * - Compile-time assertions
 */

#include <gtest/gtest.h>
#include <limits>
#include "amount.h"
#include "primitives/amount.h"

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════
// Construction Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, DefaultConstructor) {
    AmountUna amount;
    EXPECT_EQ(amount.GetUna(), 0ULL);
    EXPECT_TRUE(amount.IsZero());
}

TEST(AmountUna, ZeroConstructor) {
    AmountUna amount = AmountUna::Zero();
    EXPECT_EQ(amount.GetUna(), 0ULL);
    EXPECT_TRUE(amount.IsZero());
    EXPECT_FALSE(amount.IsPositive());
}

TEST(AmountUna, UnaConstructor) {
    AmountUna amount = AmountUna::Una(1000);
    EXPECT_EQ(amount.GetUna(), 1000ULL);
    EXPECT_FALSE(amount.IsZero());
    EXPECT_TRUE(amount.IsPositive());
}

TEST(AmountUna, DINConstructor) {
    AmountUna amount = AmountUna::DIN(1);  // 1 DIN = 100M una
    EXPECT_EQ(amount.GetUna(), 100000000ULL);
    EXPECT_EQ(amount.GetDIN(), 1ULL);
}

TEST(AmountUna, DINConstructorLarge) {
    AmountUna amount = AmountUna::DIN(1000);  // 1000 DIN
    EXPECT_EQ(amount.GetUna(), 100000000000ULL);
    EXPECT_EQ(amount.GetDIN(), 1000ULL);
}

TEST(AmountUna, MaxConstructor) {
    AmountUna amount = AmountUna::Max();
    EXPECT_EQ(amount.GetUna(), 26542800000000000ULL);  // MAX_SUPPLY_UNA
    EXPECT_TRUE(amount.IsWithinSupply());
}

// ═══════════════════════════════════════════════════════════════════
// Overflow Protection Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, AddNoOverflow) {
    AmountUna a = AmountUna::Una(1000);
    AmountUna b = AmountUna::Una(2000);

    auto result = a.Add(b);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 3000ULL);
}

TEST(AmountUna, AddOverflow) {
    AmountUna a = AmountUna::Una(UINT64_MAX - 100);
    AmountUna b = AmountUna::Una(200);

    auto result = a.Add(b);
    EXPECT_FALSE(result.has_value());  // Should overflow
}

TEST(AmountUna, AddExceedsSupply) {
    AmountUna a = AmountUna::Max();  // MAX_SUPPLY
    AmountUna b = AmountUna::Una(1);

    auto result = a.Add(b);
    EXPECT_FALSE(result.has_value());  // Exceeds MAX_SUPPLY
}

TEST(AmountUna, MulNoOverflow) {
    AmountUna amount = AmountUna::Una(1000);

    auto result = amount.Mul(5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 5000ULL);
}

TEST(AmountUna, MulOverflow) {
    AmountUna amount = AmountUna::Una(UINT64_MAX / 2);

    auto result = amount.Mul(3);
    EXPECT_FALSE(result.has_value());  // Should overflow
}

TEST(AmountUna, MulExceedsSupply) {
    AmountUna amount = AmountUna::Max();

    auto result = amount.Mul(2);
    EXPECT_FALSE(result.has_value());  // Exceeds MAX_SUPPLY
}

TEST(AmountUna, MulByZero) {
    AmountUna amount = AmountUna::Una(1000);

    auto result = amount.Mul(0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 0ULL);
}

// ═══════════════════════════════════════════════════════════════════
// Underflow Protection Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, SubNoUnderflow) {
    AmountUna a = AmountUna::Una(5000);
    AmountUna b = AmountUna::Una(2000);

    auto result = a.Sub(b);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 3000ULL);
}

TEST(AmountUna, SubUnderflow) {
    AmountUna a = AmountUna::Una(1000);
    AmountUna b = AmountUna::Una(2000);

    auto result = a.Sub(b);
    EXPECT_FALSE(result.has_value());  // Would be negative
}

TEST(AmountUna, SubExact) {
    AmountUna a = AmountUna::Una(1000);
    AmountUna b = AmountUna::Una(1000);

    auto result = a.Sub(b);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 0ULL);
}

// ═══════════════════════════════════════════════════════════════════
// Division Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, DivNormal) {
    AmountUna amount = AmountUna::Una(10000);

    auto result = amount.Div(4);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 2500ULL);
}

TEST(AmountUna, DivByZero) {
    AmountUna amount = AmountUna::Una(1000);

    auto result = amount.Div(0);
    EXPECT_FALSE(result.has_value());  // Division by zero
}

TEST(AmountUna, DivTruncates) {
    AmountUna amount = AmountUna::Una(10);

    auto result = amount.Div(3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetUna(), 3ULL);  // 10/3 = 3 (truncated)
}

// ═══════════════════════════════════════════════════════════════════
// Validation Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, IsWithinSupply) {
    AmountUna valid = AmountUna::Una(1000000);
    EXPECT_TRUE(valid.IsWithinSupply());

    AmountUna max_supply = AmountUna::Max();
    EXPECT_TRUE(max_supply.IsWithinSupply());

    AmountUna over_supply = AmountUna::UnsafeFromRaw(UINT64_MAX);
    EXPECT_FALSE(over_supply.IsWithinSupply());
}

TEST(AmountUna, IsDust) {
    AmountUna dust = AmountUna::Una(545);
    EXPECT_TRUE(dust.IsDust());

    AmountUna threshold = AmountUna::Una(546);
    EXPECT_FALSE(threshold.IsDust());  // Exactly at threshold

    AmountUna not_dust = AmountUna::Una(1000);
    EXPECT_FALSE(not_dust.IsDust());
}

TEST(AmountUna, MeetsMinFee) {
    AmountUna too_low = AmountUna::Una(99);
    EXPECT_FALSE(too_low.MeetsMinFee());

    AmountUna min_fee = AmountUna::Una(100);
    EXPECT_TRUE(min_fee.MeetsMinFee());

    AmountUna above_min = AmountUna::Una(1000);
    EXPECT_TRUE(above_min.MeetsMinFee());
}

// ═══════════════════════════════════════════════════════════════════
// Boundary Conversion Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, GetUna) {
    AmountUna amount = AmountUna::Una(123456789);
    EXPECT_EQ(amount.GetUna(), 123456789ULL);
}

TEST(AmountFormatting, FormatDINHandlesInt64Min) {
    EXPECT_EQ(FormatDIN(std::numeric_limits<int64_t>::min()), "-92233720368.54775808");
}

TEST(AmountFormatting, FormatUnaHandlesInt64Min) {
    EXPECT_EQ(FormatUna(std::numeric_limits<int64_t>::min()), "-9223372036854775808 una");
}

TEST(AmountUna, GetInt64) {
    AmountUna amount = AmountUna::Una(1000000);
    EXPECT_EQ(amount.GetInt64(), 1000000LL);
}

TEST(AmountUna, GetInt64SafeRange) {
    // MAX_SUPPLY (26.5T) < INT64_MAX (9.2 quintillion)
    AmountUna max_supply = AmountUna::Max();
    int64_t as_int64 = max_supply.GetInt64();
    EXPECT_GT(as_int64, 0);  // Should be positive
    EXPECT_EQ(static_cast<uint64_t>(as_int64), max_supply.GetUna());
}

TEST(AmountUna, GetDIN) {
    AmountUna amount = AmountUna::DIN(5);
    EXPECT_EQ(amount.GetDIN(), 5ULL);

    AmountUna fractional = AmountUna::Una(550000000);  // 5.5 DIN
    EXPECT_EQ(fractional.GetDIN(), 5ULL);  // Truncates
}

TEST(AmountUna, GetDINDouble) {
    AmountUna amount = AmountUna::Una(550000000);  // 5.5 DIN
    EXPECT_DOUBLE_EQ(amount.GetDINDouble(), 5.5);

    AmountUna exact = AmountUna::DIN(10);
    EXPECT_DOUBLE_EQ(exact.GetDINDouble(), 10.0);
}

TEST(AmountUna, UnsafeFromRaw) {
    uint64_t raw = 999999999ULL;
    AmountUna amount = AmountUna::UnsafeFromRaw(raw);
    EXPECT_EQ(amount.GetUna(), raw);
}

// ═══════════════════════════════════════════════════════════════════
// Comparison Operator Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, EqualityOperators) {
    AmountUna a = AmountUna::Una(1000);
    AmountUna b = AmountUna::Una(1000);
    AmountUna c = AmountUna::Una(2000);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_FALSE(a != b);
    EXPECT_TRUE(a != c);
}

TEST(AmountUna, ComparisonOperators) {
    AmountUna small = AmountUna::Una(100);
    AmountUna large = AmountUna::Una(1000);

    EXPECT_TRUE(small < large);
    EXPECT_TRUE(small <= large);
    EXPECT_FALSE(small > large);
    EXPECT_FALSE(small >= large);

    EXPECT_TRUE(large > small);
    EXPECT_TRUE(large >= small);
    EXPECT_FALSE(large < small);
    EXPECT_FALSE(large <= small);
}

TEST(AmountUna, ComparisonEqual) {
    AmountUna a = AmountUna::Una(1000);
    AmountUna b = AmountUna::Una(1000);

    EXPECT_TRUE(a <= b);
    EXPECT_TRUE(a >= b);
}

// ═══════════════════════════════════════════════════════════════════
// Constants Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, ConstantsZero) {
    EXPECT_EQ(amounts::ZERO.GetUna(), 0ULL);
}

TEST(AmountUna, ConstantsDustThreshold) {
    EXPECT_EQ(amounts::DUST_THRESHOLD.GetUna(), 546ULL);
}

TEST(AmountUna, ConstantsMinTxFee) {
    EXPECT_EQ(amounts::MIN_TX_FEE.GetUna(), 100ULL);
}

TEST(AmountUna, ConstantsMaxSupply) {
    EXPECT_EQ(amounts::MAX_SUPPLY.GetUna(), 26542800000000000ULL);
}

TEST(AmountUna, ConstantsOneDIN) {
    EXPECT_EQ(amounts::ONE_DIN.GetUna(), 100000000ULL);
}

// ═══════════════════════════════════════════════════════════════════
// Compile-Time Invariants (static assertions in header verified here)
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, TypeInvariants) {
    // Size check
    EXPECT_EQ(sizeof(AmountUna), 8);

    // Trivially copyable check
    EXPECT_TRUE(std::is_trivially_copyable<AmountUna>::value);

    // No implicit conversions
    EXPECT_FALSE((std::is_convertible<uint64_t, AmountUna>::value));
    EXPECT_FALSE((std::is_convertible<int64_t, AmountUna>::value));
    EXPECT_FALSE((std::is_convertible<AmountUna, uint64_t>::value));
}

// ═══════════════════════════════════════════════════════════════════
// Hash Function Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, HashFunction) {
    AmountUna a = AmountUna::Una(1000);
    AmountUna b = AmountUna::Una(1000);
    AmountUna c = AmountUna::Una(2000);

    std::hash<AmountUna> hasher;

    // Equal amounts should have equal hashes
    EXPECT_EQ(hasher(a), hasher(b));

    // Different amounts (usually) have different hashes
    // (not guaranteed, but highly likely)
    EXPECT_NE(hasher(a), hasher(c));
}

TEST(AmountUna, HashInUnorderedMap) {
    std::unordered_map<AmountUna, std::string> map;

    AmountUna key1 = AmountUna::Una(100);
    AmountUna key2 = AmountUna::Una(200);

    map[key1] = "one hundred";
    map[key2] = "two hundred";

    EXPECT_EQ(map[key1], "one hundred");
    EXPECT_EQ(map[key2], "two hundred");
    EXPECT_EQ(map.size(), 2);
}

// ═══════════════════════════════════════════════════════════════════
// Edge Case Tests
// ═══════════════════════════════════════════════════════════════════

TEST(AmountUna, DINOverflowSaturates) {
    // Attempting to create more DIN than MAX_SUPPLY should saturate
    uint64_t huge_din = UINT64_MAX / 100000000ULL + 1;
    AmountUna amount = AmountUna::DIN(huge_din);

    EXPECT_EQ(amount.GetUna(), 26542800000000000ULL);  // MAX_SUPPLY
}

TEST(AmountUna, ChainedArithmetic) {
    AmountUna start = AmountUna::Una(1000);

    auto step1 = start.Add(AmountUna::Una(500));
    ASSERT_TRUE(step1.has_value());
    EXPECT_EQ(step1->GetUna(), 1500ULL);

    auto step2 = step1->Mul(2);
    ASSERT_TRUE(step2.has_value());
    EXPECT_EQ(step2->GetUna(), 3000ULL);

    auto step3 = step2->Sub(AmountUna::Una(1000));
    ASSERT_TRUE(step3.has_value());
    EXPECT_EQ(step3->GetUna(), 2000ULL);
}

// ═══════════════════════════════════════════════════════════════════
// Test Runner
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
