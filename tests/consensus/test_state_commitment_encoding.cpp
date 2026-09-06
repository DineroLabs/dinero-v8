// state_commitment_v1 coinbase encoding — format lock.
//
// The commitment is bound to the header through the merkle root
// (commitment <- coinbase <- merkle_root <- PoW-validated header), so the
// 128-byte header is untouched and no mining implementation changes. These
// tests freeze the encoding BEFORE any activation height exists, because
// after activation a layout change is a chain split.
//
// Scope: format only. No activation height is selected and no consensus
// enforcement is wired; RequiresStateCommitment() returns false everywhere by
// construction, and one test pins that.
#include <gtest/gtest.h>

#include <cstring>

#include "consensus/state_commitment.h"
#include "mining/header_layout.h"
#include "consensus/shielded/shielded_root.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

using dinero::uint256;
using dinero::Transaction;
using dinero::consensus::BuildStateCommitmentScript;
using dinero::consensus::FindStateCommitment;
using dinero::consensus::FindStateCommitmentCandidates;
using dinero::consensus::ParseStateCommitmentScript;
using dinero::consensus::StateCommitment;
using dinero::consensus::StateCommitmentStatus;

namespace {

uint256 Root(uint8_t seed) {
    uint256 h;
    for (int i = 0; i < 32; ++i) h.data[i] = static_cast<uint8_t>(seed + i);
    return h;
}

Transaction CoinbaseWith(std::vector<std::vector<uint8_t>> scripts) {
    Transaction tx;
    tx.vin.resize(1);  // coinbase shape; the parser does not depend on it
    for (auto& s : scripts) {
        dinero::TxOutput out;
        out.scriptPubKey = std::move(s);
        tx.vout.push_back(std::move(out));
    }
    return tx;
}

std::vector<uint8_t> PayToSomething() { return {0x76, 0xa9, 0x14, 0x00, 0x88, 0xac}; }

}  // namespace

// ---------------------------------------------------------------------------
// Exact byte offsets, widths, and order.
// ---------------------------------------------------------------------------

TEST(StateCommitmentEncoding, ScriptIsExactly39BytesWithFieldsAtFixedOffsets) {
    const uint256 root = Root(0x10);
    const auto s = BuildStateCommitmentScript(root);

    ASSERT_EQ(s.size(), 39u);
    EXPECT_EQ(s.size(), StateCommitment::SCRIPT_SIZE);

    EXPECT_EQ(s[0], 0x6a) << "OP_RETURN at offset 0";
    EXPECT_EQ(s[1], 37) << "push length at offset 1 == 4 magic + 1 version + 32 root";
    EXPECT_EQ(s[2], 0x44) << "magic 'D' at offset 2";
    EXPECT_EQ(s[3], 0x4E) << "magic 'N' at offset 3";
    EXPECT_EQ(s[4], 0x52) << "magic 'R' at offset 4";
    EXPECT_EQ(s[5], 0x53) << "magic 'S' at offset 5";
    EXPECT_EQ(s[6], 0x01) << "script encoding version at offset 6";

    // Root occupies 7..38 in the SAME byte order the digest produces. A
    // reversal here would still round-trip through our own parser, so compare
    // against the raw digest bytes rather than a re-parse.
    EXPECT_EQ(0, std::memcmp(s.data() + 7, root.data, 32))
        << "root must be stored big-endian as produced, not display-reversed";
}

TEST(StateCommitmentEncoding, OffsetConstantsMatchTheProducedBytes) {
    const auto s = BuildStateCommitmentScript(Root(0x20));
    EXPECT_EQ(s[StateCommitment::OFFSET_OPRETURN], 0x6a);
    EXPECT_EQ(s[StateCommitment::OFFSET_PUSHLEN], StateCommitment::PAYLOAD_SIZE);
    EXPECT_EQ(0, std::memcmp(s.data() + StateCommitment::OFFSET_MAGIC,
                             StateCommitment::MAGIC_BYTES, 4));
    EXPECT_EQ(s[StateCommitment::OFFSET_VERSION], StateCommitment::VERSION);
    EXPECT_EQ(StateCommitment::OFFSET_ROOT + 32, StateCommitment::SCRIPT_SIZE);
}

// ---------------------------------------------------------------------------
// Round trip, and sensitivity to every single byte.
// ---------------------------------------------------------------------------

TEST(StateCommitmentEncoding, RoundTripsExactly) {
    for (uint8_t seed : {0x00, 0x01, 0x7f, 0x80, 0xff}) {
        const uint256 root = Root(seed);
        const auto parsed = ParseStateCommitmentScript(BuildStateCommitmentScript(root));
        ASSERT_TRUE(parsed.has_value()) << "seed " << int(seed);
        EXPECT_EQ(*parsed, root) << "seed " << int(seed);
    }
}

TEST(StateCommitmentEncoding, EveryRootByteAffectsTheEncoding) {
    const uint256 base = Root(0x30);
    const auto baseline = BuildStateCommitmentScript(base);
    for (int i = 0; i < 32; ++i) {
        uint256 m = base;
        m.data[i] ^= 0xff;
        const auto s = BuildStateCommitmentScript(m);
        EXPECT_NE(s, baseline) << "flipping root byte " << i << " must change the script";
        const auto parsed = ParseStateCommitmentScript(s);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, m);
        EXPECT_NE(*parsed, base);
    }
}

// ---------------------------------------------------------------------------
// Malformed input. Each case must be rejected, and a tagged-but-broken script
// must report Malformed rather than Missing — treating corruption as absence
// is how a truncation slips past a presence check.
// ---------------------------------------------------------------------------

TEST(StateCommitmentEncoding, TruncatedScriptIsRejected) {
    const auto good = BuildStateCommitmentScript(Root(0x40));
    for (size_t n = 0; n < good.size(); ++n) {
        std::vector<uint8_t> t(good.begin(), good.begin() + n);
        EXPECT_FALSE(ParseStateCommitmentScript(t).has_value())
            << "truncated to " << n << " bytes must not parse";
    }
}

TEST(StateCommitmentEncoding, TrailingBytesAreRejected) {
    auto s = BuildStateCommitmentScript(Root(0x41));
    s.push_back(0x00);
    EXPECT_FALSE(ParseStateCommitmentScript(s).has_value())
        << "a longer script that merely starts correctly is not this commitment";
}

TEST(StateCommitmentEncoding, WrongMagicVersionOrPushLengthIsRejected) {
    {
        auto s = BuildStateCommitmentScript(Root(0x42));
        s[StateCommitment::OFFSET_MAGIC + 3] = 0x46;  // "DNRF" — the filter tag
        EXPECT_FALSE(ParseStateCommitmentScript(s).has_value())
            << "the DNRF filter commitment must never parse as DNRS";
    }
    {
        auto s = BuildStateCommitmentScript(Root(0x43));
        s[StateCommitment::OFFSET_VERSION] = 0x02;
        EXPECT_FALSE(ParseStateCommitmentScript(s).has_value());
    }
    {
        auto s = BuildStateCommitmentScript(Root(0x44));
        s[StateCommitment::OFFSET_PUSHLEN] = 36;
        EXPECT_FALSE(ParseStateCommitmentScript(s).has_value());
    }
    {
        auto s = BuildStateCommitmentScript(Root(0x45));
        s[StateCommitment::OFFSET_OPRETURN] = 0x51;  // OP_1
        EXPECT_FALSE(ParseStateCommitmentScript(s).has_value());
    }
}

// ---------------------------------------------------------------------------
// Coinbase-level: exactly one.
// ---------------------------------------------------------------------------

TEST(StateCommitmentCoinbase, ExactlyOneCommitmentIsFound) {
    const uint256 root = Root(0x50);
    auto cb = CoinbaseWith({PayToSomething(), BuildStateCommitmentScript(root)});
    const auto r = FindStateCommitment(cb);
    EXPECT_EQ(r.status, StateCommitmentStatus::Ok);
    EXPECT_EQ(r.index, 1u);
    EXPECT_EQ(r.root, root);
}

TEST(StateCommitmentCoinbase, NoCommitmentReportsMissing) {
    auto cb = CoinbaseWith({PayToSomething()});
    EXPECT_EQ(FindStateCommitment(cb).status, StateCommitmentStatus::Missing);
}

TEST(StateCommitmentCoinbase, TwoCommitmentsAreRejectedNotLastWins) {
    // The DNRF precedent scans backwards and lets the last match win. Here a
    // second commitment would let one block claim two different shielded
    // states, so duplicates are an error rather than a preference.
    auto cb = CoinbaseWith({BuildStateCommitmentScript(Root(0x60)),
                            BuildStateCommitmentScript(Root(0x61))});
    const auto r = FindStateCommitment(cb);
    EXPECT_EQ(r.status, StateCommitmentStatus::Duplicate);
}

TEST(StateCommitmentCoinbase, IdenticalDuplicatesAreAlsoRejected) {
    const auto s = BuildStateCommitmentScript(Root(0x62));
    auto cb = CoinbaseWith({s, s});
    EXPECT_EQ(FindStateCommitment(cb).status, StateCommitmentStatus::Duplicate);
}

TEST(StateCommitmentCoinbase, TaggedButMalformedReportsMalformedNotMissing) {
    auto bad = BuildStateCommitmentScript(Root(0x70));
    bad.resize(bad.size() - 1);  // truncated, still carries the tag
    auto cb = CoinbaseWith({PayToSomething(), bad});
    const auto r = FindStateCommitment(cb);
    EXPECT_EQ(r.status, StateCommitmentStatus::Malformed)
        << "a corrupt commitment must not be indistinguishable from having none";
}

TEST(StateCommitmentCoinbase, FilterCommitmentIsNotMistakenForStateCommitment) {
    // DNRF and DNRS differ in one byte. Domain separation must hold.
    std::vector<uint8_t> dnrf = {0x6a, 0x25, 0x44, 0x4E, 0x52, 0x46, 0x01};
    dnrf.resize(39, 0xAB);
    auto cb = CoinbaseWith({dnrf});
    EXPECT_EQ(FindStateCommitment(cb).status, StateCommitmentStatus::Missing);
    EXPECT_TRUE(FindStateCommitmentCandidates(cb).empty());
}

// ---------------------------------------------------------------------------
// Pre-activation posture.
// ---------------------------------------------------------------------------

TEST(StateCommitmentActivation, NoHeightRequiresTheCommitmentYet) {
    // Format work must not activate anything. If this ever fails, enforcement
    // was enabled outside the separate reviewed change that should own it.
    for (uint64_t h : {uint64_t{0}, uint64_t{1}, uint64_t{61000}, uint64_t{99677},
                       uint64_t{1000000}, UINT64_MAX}) {
        EXPECT_FALSE(dinero::consensus::RequiresStateCommitment(h))
            << "height " << h << " must not require a commitment during format work";
    }
    EXPECT_EQ(StateCommitment::kActivationHeightUnset, UINT64_MAX);
}

TEST(StateCommitmentActivation, ParsingIsAdvisoryAndDoesNotDependOnHeight) {
    // The parser is pure: same script, same answer, no height input at all.
    const auto s = BuildStateCommitmentScript(Root(0x80));
    EXPECT_TRUE(ParseStateCommitmentScript(s).has_value());
}

// ---------------------------------------------------------------------------
// Domain separation from the digest preimage.
// ---------------------------------------------------------------------------

TEST(StateCommitmentEncoding, CoinbaseMagicDiffersFromThePreimageTag) {
    // SHR1 tags the digest preimage; DNRS tags the coinbase encoding. A value
    // from one domain must never be readable as the other.
    const char* shr1 = dinero::consensus::shielded::SHIELDED_ROOT_TAG;
    EXPECT_NE(0, std::memcmp(StateCommitment::MAGIC_BYTES, shr1, 4));
}

// ---------------------------------------------------------------------------
// The header is untouched — the whole point of choosing coinbase placement.
// ---------------------------------------------------------------------------

TEST(StateCommitmentHeader, WireHeaderStaysExactly128BytesBeforeAndAfterActivation) {
    // Coinbase placement means the serialized header length is invariant across
    // activation: there is no "before" and "after" length. If this ever fails,
    // someone moved the commitment into the header and every CPU/GPU/SV2 miner,
    // template, and PoW serialization assumption breaks with it.
    EXPECT_EQ(DINERO_HEADER_SIZE_BYTES, 128);
    EXPECT_EQ(sizeof(dinero::mining::BlockHeaderV1), 128u);
}

TEST(StateCommitmentHeader, LegacyHeaderFieldOffsetsAreUnchanged) {
    EXPECT_EQ(DINERO_HEADER_VERSION_OFFSET, 0);
    EXPECT_EQ(DINERO_HEADER_PREVHASH_OFFSET, 4);
    EXPECT_EQ(DINERO_HEADER_MERKLEROOT_OFFSET, 36);
    EXPECT_EQ(DINERO_HEADER_UTREEXO_OFFSET, 68);
    EXPECT_EQ(DINERO_HEADER_TIMESTAMP_OFFSET, 100);
    EXPECT_EQ(DINERO_HEADER_DIFFICULTY_OFFSET, 108);
    EXPECT_EQ(DINERO_HEADER_NONCE_OFFSET, 112);
    EXPECT_EQ(DINERO_HEADER_RESERVED_OFFSET, 116);
}

TEST(StateCommitmentHeader, ReservedBytesRemainReservedAndUnused) {
    // The rejected placement option would have consumed these 12 bytes. They
    // must stay consensus-zero and untouched by this work, so that option
    // remains available and, more importantly, so no partial encoding leaks in.
    EXPECT_EQ(DINERO_HEADER_RESERVED_OFFSET + 12, DINERO_HEADER_SIZE_BYTES);
    const auto s = BuildStateCommitmentScript(Root(0x90));
    EXPECT_EQ(s.size(), 39u) << "the commitment lives in a script, never in the header";
}
