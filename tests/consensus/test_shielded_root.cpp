// Copyright (c) 2026 Dinero Labs.
//
// Tests for the shielded-state root — the fingerprint a future block-header
// commitment would carry. These are consensus-shaped tests: they assert on the
// exact preimage layout, not just that "a hash comes out", because once this
// is committed in a header, a layout change IS a chain split.

#include <gtest/gtest.h>

#include "consensus/shielded/shielded_root.h"

using namespace dinero::consensus::shielded;

namespace {

std::vector<uint8_t> Root32(uint8_t fill) { return std::vector<uint8_t>(32, fill); }

}  // namespace

// ── layout ────────────────────────────────────────────────────────────────

TEST(ShieldedRoot, PreimageStartsWithTagAndVersion) {
    const auto pre = BuildShieldedRootPreimage(Root32(0xAA), 7, {1, 2}, {3, 4, 5});
    ASSERT_GE(pre.size(), 5u);
    EXPECT_EQ(pre[0], 'S');
    EXPECT_EQ(pre[1], 'H');
    EXPECT_EQ(pre[2], 'R');
    EXPECT_EQ(pre[3], '1');
    EXPECT_EQ(pre[4], SHIELDED_ROOT_VERSION);
}

TEST(ShieldedRoot, PreimageHasTheDocumentedSize) {
    // 4 tag + 1 version + 32 root + 8 size + 8 nf_len + 2 nf + 8 ah_len + 3 ah
    const auto pre = BuildShieldedRootPreimage(Root32(0xAA), 7, {1, 2}, {3, 4, 5});
    EXPECT_EQ(pre.size(), 4u + 1u + 32u + 8u + 8u + 2u + 8u + 3u);
}

// A root that is not 32 bytes must be zero-filled, never truncated or skipped —
// skipping would shift every later field and silently change the meaning.
TEST(ShieldedRoot, NonStandardRootLengthIsZeroFilledNotSkipped) {
    const auto short_root = BuildShieldedRootPreimage({1, 2, 3}, 0, {}, {});
    const auto zero_root = BuildShieldedRootPreimage(std::vector<uint8_t>(32, 0), 0, {}, {});
    EXPECT_EQ(short_root, zero_root);
    EXPECT_EQ(short_root.size(), 4u + 1u + 32u + 8u + 8u + 8u);
}

// ── every field is actually covered ───────────────────────────────────────

TEST(ShieldedRoot, EveryFieldChangesTheDigest) {
    const auto base = ComputeShieldedRootFromParts(Root32(0x11), 5, {9}, {8});
    EXPECT_NE(base, ComputeShieldedRootFromParts(Root32(0x12), 5, {9}, {8})) << "tree root";
    EXPECT_NE(base, ComputeShieldedRootFromParts(Root32(0x11), 6, {9}, {8})) << "tree size";
    EXPECT_NE(base, ComputeShieldedRootFromParts(Root32(0x11), 5, {7}, {8})) << "nullifiers";
    EXPECT_NE(base, ComputeShieldedRootFromParts(Root32(0x11), 5, {9}, {6})) << "anchors";
}

TEST(ShieldedRoot, IsDeterministic) {
    EXPECT_EQ(ComputeShieldedRootFromParts(Root32(0x11), 5, {9}, {8}),
              ComputeShieldedRootFromParts(Root32(0x11), 5, {9}, {8}));
}

// ── THE canonical-encoding test ───────────────────────────────────────────
//
// This is the reason this function does not simply reuse DSR2's layout. DSR2
// concatenates nullifier content and anchor bytes with no length framing, so
// these two distinct states share a preimage tail and can collide. With
// explicit lengths they cannot.
TEST(ShieldedRoot, SectionBoundaryIsUnambiguous) {
    // Same concatenation {1,2,3}, different split between the two sections.
    const auto a = ComputeShieldedRootFromParts(Root32(0), 0, {1, 2}, {3});
    const auto b = ComputeShieldedRootFromParts(Root32(0), 0, {1}, {2, 3});
    EXPECT_NE(a, b) << "moving a byte across the section boundary must change the digest";
}

TEST(ShieldedRoot, EmptySectionsAreDistinctFromAbsentOnes) {
    const auto empty_nf = ComputeShieldedRootFromParts(Root32(0), 0, {}, {1});
    const auto empty_ah = ComputeShieldedRootFromParts(Root32(0), 0, {1}, {});
    EXPECT_NE(empty_nf, empty_ah);
}

// ── the forest must NOT be in here ────────────────────────────────────────
//
// The whole point of extracting this from DSR2: header.utreexo_root already
// commits the forest. Including it again would make this consensus value
// hostage to forest serialization.
TEST(ShieldedRoot, DigestDependsOnlyOnTheFourShieldedInputs) {
    // Two calls with identical shielded inputs must agree regardless of any
    // ambient forest state — there is no forest parameter to pass, which is
    // the structural guarantee. This locks the signature against a future
    // well-meaning "just reuse DSR2" refactor.
    const auto x = ComputeShieldedRootFromParts(Root32(0x33), 42, {1, 2, 3}, {4, 5});
    const auto y = ComputeShieldedRootFromParts(Root32(0x33), 42, {1, 2, 3}, {4, 5});
    EXPECT_EQ(x, y);
}

// ── forgery detection: the attacks this commitment must stop ──────────────
//
// These are the reasons the shielded half needs a chain binding at all. A
// snapshot's UTXO half is already bound by header.utreexo_root; forging the
// shielded half is what remains possible today.

// THE attack. Drop a nullifier from a snapshot and a previously-spent shielded
// note looks unspent — a shielded double-spend. The commitment must not be
// blind to it, at any position in the set.
TEST(ShieldedRoot, OmittingAnyNullifierChangesTheRoot) {
    const std::vector<uint8_t> full = {10, 11, 12, 13, 14, 15, 16, 17};
    const auto honest = ComputeShieldedRootFromParts(Root32(1), 4, full, {9, 9});
    for (size_t drop = 0; drop < full.size(); ++drop) {
        std::vector<uint8_t> tampered = full;
        tampered.erase(tampered.begin() + static_cast<long>(drop));
        EXPECT_NE(honest, ComputeShieldedRootFromParts(Root32(1), 4, tampered, {9, 9}))
            << "dropping nullifier byte " << drop << " went undetected";
    }
}

// Truncating the set (dropping the tail — the cheapest omission) must be caught
// by the length prefix even before the content differs.
TEST(ShieldedRoot, TruncatedNullifierSetIsDetected) {
    const std::vector<uint8_t> full = {1, 2, 3, 4, 5, 6};
    const auto honest = ComputeShieldedRootFromParts(Root32(1), 4, full, {7});
    for (size_t keep = 0; keep < full.size(); ++keep) {
        const std::vector<uint8_t> shorter(full.begin(), full.begin() + static_cast<long>(keep));
        EXPECT_NE(honest, ComputeShieldedRootFromParts(Root32(1), 4, shorter, {7}))
            << "truncating to " << keep << " bytes went undetected";
    }
}

// Anchor-history forgery: inserting a fake anchor lets a spend prove against a
// tree state that never existed on the real chain — minting from nothing.
TEST(ShieldedRoot, InjectedAnchorChangesTheRoot) {
    const std::vector<uint8_t> anchors = {1, 2, 3, 4};
    const auto honest = ComputeShieldedRootFromParts(Root32(1), 4, {5}, anchors);
    std::vector<uint8_t> injected = anchors;
    injected.push_back(0xFF);  // one extra anchor entry
    EXPECT_NE(honest, ComputeShieldedRootFromParts(Root32(1), 4, {5}, injected));
}

// Canonical ordering is load-bearing. NullifierSet::SerializeContent() emits
// entries sorted by (height ASC, nullifier ASC); if a tampered snapshot could
// reorder them and still hash the same, the ordering guarantee would be doing
// no work. This is the property a canonical nullifier accumulator must keep.
TEST(ShieldedRoot, ReorderedNullifierContentChangesTheRoot) {
    const auto ordered = ComputeShieldedRootFromParts(Root32(1), 4, {1, 2, 3, 4}, {9});
    const auto swapped = ComputeShieldedRootFromParts(Root32(1), 4, {3, 4, 1, 2}, {9});
    EXPECT_NE(ordered, swapped);
}

// Content moved between the two sections must not cancel out — the same total
// bytes split differently is different state.
TEST(ShieldedRoot, ContentCannotBeShuffledBetweenSections) {
    const auto a = ComputeShieldedRootFromParts(Root32(1), 4, {1, 2, 3, 4}, {5, 6});
    const auto b = ComputeShieldedRootFromParts(Root32(1), 4, {1, 2, 3}, {4, 5, 6});
    const auto c = ComputeShieldedRootFromParts(Root32(1), 4, {1, 2, 3, 4, 5}, {6});
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(b, c);
}

// ── regression lock ───────────────────────────────────────────────────────
//
// Pins the digest of the all-empty state. If this value ever changes, the
// layout changed — which after activation would be a chain split. Failing
// here is the intended alarm, not an inconvenience.
TEST(ShieldedRoot, EmptyStateDigestIsPinned) {
    const auto d = ComputeShieldedRootFromParts(std::vector<uint8_t>(32, 0), 0, {}, {});
    EXPECT_EQ(d.GetHex(), "d4c4c8cab0145d981b138bd960fbb9437a537f2630a840c5916c24139ffbbbd2");
}
