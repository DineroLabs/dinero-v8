// Externally fixed golden vectors — the guardrail against a coordinated
// writer + parser + golden-file reversal.
//
// Every other vector in this tree is produced BY the production serializer.
// That is fine for detecting drift, but it cannot detect a change that is
// wrong and self-consistent: flip the writer, flip the parser, regenerate the
// golden file with --update, and all three agree while the bytes on the wire
// are reversed. Demonstrated by mutation on the canonical suite -- writer and
// parser both reversed leaves 7 of 8 tests passing.
//
// The constants below were computed by an INDEPENDENT implementation written
// from the specification text (docs/specs/state_commitment_v1.md) in Python,
// using hashlib, without calling, linking, or reading the C++ serializer.
// They are pasted here as literals. Regenerating them requires a human to
// re-derive them from the spec, which is exactly the friction we want: a
// coordinated code change cannot update them as a side effect.
//
// If one of these fails, do NOT "fix" the constant. Either the encoding
// changed (which after activation is a chain split and must bump a version),
// or the spec and the code have diverged.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "consensus/shielded/nullifier_accumulator.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/state_commitment.h"
#include "primitives/uint256.h"

namespace sh = dinero::consensus::shielded;
using dinero::uint256;

namespace {

// --- externally computed constants (python/hashlib, from the spec) ---------
//
// inputs: tree_root = 32 x 0x11
//         tree_size = 1
//         nullifier_accumulator = 32 x 0x22
//         anchor_bytes = AA BB CC DD
constexpr const char* kExtPreimageHex =
    "53485231"                                                          // "SHR1"
    "02"                                                                // version 2
    "1111111111111111111111111111111111111111111111111111111111111111"  // tree root
    "0100000000000000"                                                  // size = 1, LE64
    "2222222222222222222222222222222222222222222222222222222222222222"  // nullifier acc
    "0400000000000000"                                                  // anchor len = 4, LE64
    "aabbccdd";                                                         // anchor bytes
constexpr size_t kExtPreimageLen = 89;

constexpr const char* kExtShieldedRootHex =
    "2da0573f9de2ce63213d16fe8086e694559ed7d9624136edae23c475a31a212a";

// entries: (height 1, 32 x 0x01), (height 2, 32 x 0x02)
constexpr const char* kExtNullifierAccHex =
    "6040a7ca966412596dbf20599512777479bb74bfbbd809088064ec7760033cae";

// coinbase script committing to kExtShieldedRootHex
constexpr const char* kExtCoinbaseScriptHex =
    "6a25444e525301"                                                    // OP_RETURN|37|DNRS|v1
    "2da0573f9de2ce63213d16fe8086e694559ed7d9624136edae23c475a31a212a";

std::string ToHex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s.push_back(d[b >> 4]); s.push_back(d[b & 15]); }
    return s;
}
std::string ToHex(const uint8_t* p, size_t n) {
    return ToHex(std::vector<uint8_t>(p, p + n));
}

std::vector<uint8_t> Fill(size_t n, uint8_t b) { return std::vector<uint8_t>(n, b); }

uint256 U256Fill(uint8_t b) {
    uint256 h;
    std::memset(h.data, b, 32);
    return h;
}

}  // namespace

TEST(ExternalVectors, PreimageMatchesTheIndependentImplementation) {
    const auto pre = sh::BuildShieldedRootPreimage(Fill(32, 0x11), 1, U256Fill(0x22),
                                                   {0xAA, 0xBB, 0xCC, 0xDD});
    EXPECT_EQ(pre.size(), kExtPreimageLen);
    EXPECT_EQ(ToHex(pre), std::string(kExtPreimageHex))
        << "the SHR1 preimage layout no longer matches the specification";
}

TEST(ExternalVectors, ShieldedRootDigestMatchesTheIndependentImplementation) {
    const auto d = sh::ComputeShieldedRootFromParts(Fill(32, 0x11), 1, U256Fill(0x22),
                                                    {0xAA, 0xBB, 0xCC, 0xDD});
    // Compare RAW bytes, not GetHex(). GetHex() reverses, so comparing display
    // strings here would accept a reversed digest and defeat the guardrail.
    EXPECT_EQ(ToHex(d.data, 32), std::string(kExtShieldedRootHex));
}

TEST(ExternalVectors, NullifierAccumulatorMatchesTheIndependentImplementation) {
    std::vector<sh::NullifierEntry> entries(2);
    entries[0].height = 1;
    entries[0].nullifier.fill(0x01);
    entries[1].height = 2;
    entries[1].nullifier.fill(0x02);
    const auto d = sh::ComputeNullifierAccumulator(entries);
    EXPECT_EQ(ToHex(d.data, 32), std::string(kExtNullifierAccHex));
}

TEST(ExternalVectors, CoinbaseScriptMatchesTheIndependentImplementation) {
    uint256 root;
    for (int i = 0; i < 32; ++i) {
        const std::string byte_hex(kExtShieldedRootHex + i * 2, 2);
        root.data[i] = static_cast<uint8_t>(std::stoul(byte_hex, nullptr, 16));
    }
    const auto script = dinero::consensus::BuildStateCommitmentScript(root);
    EXPECT_EQ(ToHex(script), std::string(kExtCoinbaseScriptHex))
        << "a writer+parser reversal would round-trip cleanly but fail HERE";
}

TEST(ExternalVectors, TheGuardrailIsNotSelfReferential) {
    // Guard against the guardrail decaying: these constants must be literals,
    // never re-derived from the production code at runtime. If the digest were
    // recomputed and compared to itself the test would pass under any
    // consistent reversal. Pin the literal so that regression is visible.
    EXPECT_EQ(std::string(kExtShieldedRootHex).size(), 64u);
    EXPECT_EQ(std::string(kExtCoinbaseScriptHex).size(), 78u)  // 39 bytes
        << "script must be exactly 39 bytes: OP_RETURN + push + 37";
    EXPECT_EQ(std::string(kExtCoinbaseScriptHex).substr(14),
              std::string(kExtShieldedRootHex))
        << "the script's root field must be the digest in the SAME order";
}
