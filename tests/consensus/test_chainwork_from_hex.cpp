// Regression test for the snapshot-bootstrap deadlock (PR #390):
// ChainworkFromHex length-guards to 64 chars but historically fed each
// 16-char word to std::stoull WITHOUT a try/catch. A 64-char string that
// is NOT all hex digits made stoull throw std::invalid_argument; uncaught,
// that aborted LoadSnapshot before StartBackgroundValidation() ran,
// wedging a fresh snapshot node (tip held at base, validation NotStarted).
//
// The fix validates every char is a hex digit before parsing and returns
// zero work for a malformed string. This test pins that a 64-char non-hex
// input returns zero and NEVER throws — it fails (throws/aborts) without
// the fix.

#include "consensus/chainwork.h"
#include <gtest/gtest.h>
#include <string>

using namespace dinero;

TEST(ChainworkFromHex, ValidHexParsesNonZero) {
    // A well-formed 64-char chainwork string parses to non-zero work.
    std::string valid(63, '0');
    valid += "1";  // ...0001
    arith_uint256 w = ChainworkFromHex(valid);
    EXPECT_NE(w, arith_uint256());
}

TEST(ChainworkFromHex, MalformedNonHexReturnsZeroWithoutThrowing) {
    // 64 chars but not hex (would make std::stoull throw pre-fix).
    const std::string non_hex(64, 'z');
    arith_uint256 out;
    EXPECT_NO_THROW({ out = ChainworkFromHex(non_hex); });
    EXPECT_EQ(out, arith_uint256());

    // Mixed valid-hex prefix + garbage in a later 16-char word — the throw
    // used to originate from the second word, so a prefix-only guard would
    // miss it. Full-string validation catches it.
    std::string mixed(48, 'a');   // 3 valid words
    mixed += "notvalidhexchars";  // 4th word: 16 non-hex chars
    ASSERT_EQ(mixed.size(), 64u);
    EXPECT_NO_THROW({ out = ChainworkFromHex(mixed); });
    EXPECT_EQ(out, arith_uint256());
}

TEST(ChainworkFromHex, WrongLengthReturnsZero) {
    EXPECT_EQ(ChainworkFromHex(""), arith_uint256());
    EXPECT_EQ(ChainworkFromHex(std::string(63, 'a')), arith_uint256());
    EXPECT_EQ(ChainworkFromHex(std::string(65, 'a')), arith_uint256());
}
