// Copyright (c) 2026 Dinero Labs.
//
// The canonical nullifier accumulator. Consensus-shaped tests: the ordering
// rule and the fail-closed contract are asserted directly, because both are
// properties a block-header commitment would depend on.

#include <gtest/gtest.h>

#include <algorithm>

#include "consensus/shielded/nullifier_accumulator.h"

using namespace dinero::consensus::shielded;

namespace {
NullifierEntry E(uint32_t h, uint8_t fill) {
    NullifierEntry e;
    e.height = h;
    e.nullifier.fill(fill);
    return e;
}
}  // namespace

// ── the ordering rule, stated directly ────────────────────────────────────

TEST(NullifierAccumulator, OrdersByHeightThenNullifierBytes) {
    EXPECT_TRUE(NullifierEntryLess(E(1, 0xFF), E(2, 0x00))) << "height dominates";
    EXPECT_TRUE(NullifierEntryLess(E(5, 0x01), E(5, 0x02))) << "then nullifier bytes";
    EXPECT_FALSE(NullifierEntryLess(E(5, 0x02), E(5, 0x01)));
    EXPECT_FALSE(NullifierEntryLess(E(5, 0x01), E(5, 0x01))) << "irreflexive";
}

// Bytes compare as UNSIGNED octets. If they were ever compared as signed char,
// 0x80..0xFF would sort below 0x00..0x7F and two nodes could disagree.
TEST(NullifierAccumulator, NullifierBytesCompareAsUnsigned) {
    EXPECT_TRUE(NullifierEntryLess(E(1, 0x7F), E(1, 0x80)));
    EXPECT_FALSE(NullifierEntryLess(E(1, 0x80), E(1, 0x7F)));
}

// ── canonicality: input order must not matter ─────────────────────────────

TEST(NullifierAccumulator, InputOrderDoesNotAffectTheDigest) {
    std::vector<NullifierEntry> v{E(3, 0x33), E(1, 0x11), E(2, 0x22), E(1, 0x99)};
    const auto expected = ComputeNullifierAccumulator(v);
    std::sort(v.begin(), v.end(),
              [](const NullifierEntry& a, const NullifierEntry& b) { return b.height < a.height; });
    EXPECT_EQ(expected, ComputeNullifierAccumulator(v)) << "reverse-height order";
    std::reverse(v.begin(), v.end());
    EXPECT_EQ(expected, ComputeNullifierAccumulator(v)) << "reversed again";
    std::rotate(v.begin(), v.begin() + 1, v.end());
    EXPECT_EQ(expected, ComputeNullifierAccumulator(v)) << "rotated";
}

// It is a SET. The same pair twice is the same fact.
TEST(NullifierAccumulator, DuplicateEntriesCollapse) {
    const auto once = ComputeNullifierAccumulator({E(1, 0x11), E(2, 0x22)});
    const auto twice = ComputeNullifierAccumulator({E(1, 0x11), E(2, 0x22), E(1, 0x11)});
    EXPECT_EQ(once, twice);
}

// ── every entry is covered ────────────────────────────────────────────────

TEST(NullifierAccumulator, DroppingAnyEntryChangesTheDigest) {
    const std::vector<NullifierEntry> full{E(1, 0x11), E(2, 0x22), E(3, 0x33), E(4, 0x44)};
    const auto honest = ComputeNullifierAccumulator(full);
    for (size_t i = 0; i < full.size(); ++i) {
        std::vector<NullifierEntry> missing = full;
        missing.erase(missing.begin() + static_cast<long>(i));
        EXPECT_NE(honest, ComputeNullifierAccumulator(missing)) << "dropping entry " << i;
    }
}

TEST(NullifierAccumulator, HeightIsPartOfTheCommitment) {
    EXPECT_NE(ComputeNullifierAccumulator({E(1, 0xAB)}),
              ComputeNullifierAccumulator({E(2, 0xAB)}));
}

// The count is committed, so an entry cannot be dropped and another duplicated
// to keep the byte length the same.
TEST(NullifierAccumulator, CountIsCommitted) {
    EXPECT_NE(ComputeNullifierAccumulator({E(1, 0x11), E(2, 0x22)}),
              ComputeNullifierAccumulator({E(1, 0x11)}));
}

TEST(NullifierAccumulator, EmptySetHasItsOwnDigest) {
    const auto empty = ComputeNullifierAccumulator({});
    EXPECT_NE(empty, ComputeNullifierAccumulator({E(0, 0x00)}));
}

// ── fail-closed ───────────────────────────────────────────────────────────
//
// THE contract. SerializeContent() signals a read error by returning empty
// bytes, which hash to the empty-set digest — so a local database fault
// produces the same commitment as an attacker who deleted every nullifier.
// Accumulate must be unable to express that confusion.
TEST(NullifierAccumulator, UnreadableSetYieldsNulloptNotTheEmptyDigest) {
    NullifierSet closed;  // never opened: enumeration cannot succeed
    const auto got = AccumulateNullifierSet(closed);
    ASSERT_FALSE(got.has_value())
        << "an unreadable set must not be reported as any digest at all";
}

// ── regression lock ───────────────────────────────────────────────────────
TEST(NullifierAccumulator, EmptyDigestIsPinned) {
    EXPECT_EQ(ComputeNullifierAccumulator({}).GetHex(), "4f760a0ef5ff321a3eaa0feca778ff19c9126aefd506b91732d747495df62696");
}
