// Test: BIP22 bits formatting for getblocktemplate
// Ensures compact difficulty bits are always formatted as 8-char lowercase hex without prefix

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <cctype>

// Helper function to format bits (same logic as main.cpp GBT handler)
static std::string FormatBitsForBIP22(uint32_t bits) {
    char bits_hex[9];
    std::snprintf(bits_hex, sizeof(bits_hex), "%08x", bits);
    return std::string(bits_hex);
}

TEST(GbtBitsFormat, EightLowerHexNoPrefix) {
    // Phase 1 easy difficulty: 0x1d3fffff
    auto s = FormatBitsForBIP22(0x1d3fffff);
    ASSERT_EQ(s.size(), 8u);

    // All characters must be lowercase hex (no 0x prefix, no uppercase)
    for (char c : s) {
        ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Invalid hex character: " << c;
    }

    EXPECT_EQ(s, "1d3fffff");
}

TEST(GbtBitsFormat, MultipleCompactBitsValues) {
    // Test various compact bits values to ensure consistent formatting
    struct TestCase {
        uint32_t bits;
        std::string expected;
    };

    std::vector<TestCase> cases = {
        {0x1d3fffff, "1d3fffff"},  // Phase 1 easy
        {0x1d00ffff, "1d00ffff"},  // Legacy incorrect value (for comparison)
        {0x1a123456, "1a123456"},  // Arbitrary ASERT value
        {0x207fffff, "207fffff"},  // Higher difficulty
        {0x1b0404cb, "1b0404cb"},  // Bitcoin genesis difficulty
        {0x00000000, "00000000"},  // Edge case: zero
        {0xffffffff, "ffffffff"},  // Edge case: max uint32
    };

    for (const auto& tc : cases) {
        auto result = FormatBitsForBIP22(tc.bits);
        EXPECT_EQ(result, tc.expected)
            << "bits=0x" << std::hex << tc.bits;
        EXPECT_EQ(result.size(), 8u);

        // Verify no uppercase or invalid chars
        for (char c : result) {
            ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    }
}

TEST(GbtBitsFormat, NoPrefix) {
    auto s = FormatBitsForBIP22(0x1d3fffff);

    // Must not start with "0x" or "0X"
    EXPECT_NE(s.substr(0, 2), "0x");
    EXPECT_NE(s.substr(0, 2), "0X");

    // First two chars should be the actual hex value
    EXPECT_EQ(s.substr(0, 2), "1d");
}

TEST(GbtBitsFormat, LeadingZeroPreservation) {
    // Bits with leading zeros must preserve all 8 characters
    auto s = FormatBitsForBIP22(0x00ab12cd);
    EXPECT_EQ(s, "00ab12cd");
    EXPECT_EQ(s.size(), 8u);

    // All zeros
    auto zero = FormatBitsForBIP22(0x00000000);
    EXPECT_EQ(zero, "00000000");
    EXPECT_EQ(zero.size(), 8u);
}
