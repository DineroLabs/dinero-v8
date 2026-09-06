// Copyright (c) 2026 Dinero Labs.
//
// Tests for the shielded-state root — the fingerprint a future block-header
// commitment would carry. These are consensus-shaped tests: they assert on the
// exact preimage layout, not just that "a hash comes out", because once this
// is committed in a header, a layout change IS a chain split.

#include <gtest/gtest.h>

#include "consensus/shielded/nullifier_accumulator.h"
#include "consensus/shielded/shielded_root.h"

using namespace dinero::consensus::shielded;

namespace {

std::vector<uint8_t> Root32(uint8_t fill) { return std::vector<uint8_t>(32, fill); }

// v2 takes the nullifier ACCUMULATOR digest, not raw content. These helpers
// turn the old byte-vector fixtures into entry sets so each test keeps
// asserting the same property it always did.
dinero::uint256 Acc(const std::vector<uint8_t>& bytes) {
    std::vector<NullifierEntry> v;
    for (size_t i = 0; i < bytes.size(); ++i) {
        NullifierEntry e;
        e.height = static_cast<uint32_t>(i);
        e.nullifier.fill(bytes[i]);
        v.push_back(e);
    }
    return ComputeNullifierAccumulator(v);
}

// Unwrapping helpers. BuildShieldedRootPreimage and ComputeShieldedRootFromParts
// now REJECT a tree root that is not exactly 32 bytes, so they return optionals.
// Every fixture below passes a valid 32-byte root, so these assert the value is
// present and hand back the payload -- keeping each test about the property it
// was written for. The rejection itself is tested separately.
std::vector<uint8_t> Pre(const std::vector<uint8_t>& root, uint64_t size,
                         const dinero::uint256& acc, const std::vector<uint8_t>& ah) {
    const auto p = BuildShieldedRootPreimage(root, size, acc, ah);
    EXPECT_TRUE(p.has_value()) << "fixture root must be 32 bytes";
    return p.value_or(std::vector<uint8_t>{});
}

dinero::uint256 Digest(const std::vector<uint8_t>& root, uint64_t size,
                       const dinero::uint256& acc, const std::vector<uint8_t>& ah) {
    const auto d = ComputeShieldedRootFromParts(root, size, acc, ah);
    EXPECT_TRUE(d.has_value()) << "fixture root must be 32 bytes";
    return d.value_or(dinero::uint256());
}

}  // namespace

// ── layout ────────────────────────────────────────────────────────────────

TEST(ShieldedRoot, PreimageStartsWithTagAndVersion) {
    const auto pre = Pre(Root32(0xAA), 7, Acc({1, 2}), {3, 4, 5});
    ASSERT_GE(pre.size(), 5u);
    EXPECT_EQ(pre[0], 'S');
    EXPECT_EQ(pre[1], 'H');
    EXPECT_EQ(pre[2], 'R');
    EXPECT_EQ(pre[3], '1');
    EXPECT_EQ(pre[4], SHIELDED_ROOT_VERSION);
}

TEST(ShieldedRoot, PreimageHasTheDocumentedSize) {
    // 4 tag + 1 version + 32 root + 8 size + 8 nf_len + 2 nf + 8 ah_len + 3 ah
    const auto pre = Pre(Root32(0xAA), 7, Acc({1, 2}), {3, 4, 5});
    EXPECT_EQ(pre.size(), 4u + 1u + 32u + 8u + 32u + 8u + 3u);
}

// A root that is not 32 bytes is REJECTED.
//
// This test previously asserted the opposite — that a short root was
// zero-filled — and that is the defect it now locks out. Zero-filling made a
// corrupt or truncated root produce byte-for-byte the same commitment as a
// genuinely empty tree, so two different shielded states committed to one
// value. Nothing downstream can recover the difference afterwards, which is
// why there is no length at which padding is the safe choice.
TEST(ShieldedRoot, NonStandardRootLengthIsRejectedNotZeroFilled) {
    const auto zero_root = BuildShieldedRootPreimage(std::vector<uint8_t>(32, 0), 0, Acc({}), {});
    ASSERT_TRUE(zero_root.has_value()) << "a real 32-byte root is still accepted";

    for (const size_t bad_len : {size_t{0}, size_t{1}, size_t{3}, size_t{31},
                                 size_t{33}, size_t{64}}) {
        const std::vector<uint8_t> bad(bad_len, 0x5A);
        const auto got = BuildShieldedRootPreimage(bad, 0, Acc({}), {});
        EXPECT_FALSE(got.has_value())
            << "a " << bad_len << "-byte root must be rejected, not padded";
        EXPECT_FALSE(ComputeShieldedRootFromParts(bad, 0, Acc({}), {}).has_value())
            << "a " << bad_len << "-byte root must not yield a digest";
    }

    // The specific confusion that mattered: a bad root must not be able to
    // impersonate the empty tree.
    const auto empty_tree = ComputeShieldedRootFromParts(std::vector<uint8_t>(32, 0),
                                                        0, Acc({}), {});
    ASSERT_TRUE(empty_tree.has_value());
    EXPECT_FALSE(ComputeShieldedRootFromParts({1, 2, 3}, 0, Acc({}), {}).has_value())
        << "a truncated root must not hash as the empty tree";
}

// ── every field is actually covered ───────────────────────────────────────

TEST(ShieldedRoot, EveryFieldChangesTheDigest) {
    const auto base = Digest(Root32(0x11), 5, Acc({9}), {8});
    EXPECT_NE(base, Digest(Root32(0x12), 5, Acc({9}), {8})) << "tree root";
    EXPECT_NE(base, Digest(Root32(0x11), 6, Acc({9}), {8})) << "tree size";
    EXPECT_NE(base, Digest(Root32(0x11), 5, Acc({7}), {8})) << "nullifiers";
    EXPECT_NE(base, Digest(Root32(0x11), 5, Acc({9}), {6})) << "anchors";
}

TEST(ShieldedRoot, IsDeterministic) {
    EXPECT_EQ(Digest(Root32(0x11), 5, Acc({9}), {8}),
              Digest(Root32(0x11), 5, Acc({9}), {8}));
}

// ── THE canonical-encoding test ───────────────────────────────────────────
//
// This is the reason this function does not simply reuse DSR2's layout. DSR2
// concatenates nullifier content and anchor bytes with no length framing, so
// these two distinct states share a preimage tail and can collide. With
// explicit lengths they cannot.
TEST(ShieldedRoot, SectionBoundaryIsUnambiguous) {
    // Same concatenation {1,2,3}, different split between the two sections.
    const auto a = Digest(Root32(0), 0, Acc({1, 2}), {3});
    const auto b = Digest(Root32(0), 0, Acc({1}), {2, 3});
    EXPECT_NE(a, b) << "moving a byte across the section boundary must change the digest";
}

TEST(ShieldedRoot, EmptySectionsAreDistinctFromAbsentOnes) {
    const auto empty_nf = Digest(Root32(0), 0, Acc({}), {1});
    const auto empty_ah = Digest(Root32(0), 0, Acc({1}), {});
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
    const auto x = Digest(Root32(0x33), 42, Acc({1, 2, 3}), {4, 5});
    const auto y = Digest(Root32(0x33), 42, Acc({1, 2, 3}), {4, 5});
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
    const auto honest = Digest(Root32(1), 4, Acc(full), {9, 9});
    for (size_t drop = 0; drop < full.size(); ++drop) {
        std::vector<uint8_t> tampered = full;
        tampered.erase(tampered.begin() + static_cast<long>(drop));
        EXPECT_NE(honest, Digest(Root32(1), 4, Acc(tampered), {9, 9}))
            << "dropping nullifier byte " << drop << " went undetected";
    }
}

// Truncating the set (dropping the tail — the cheapest omission) must be caught
// by the length prefix even before the content differs.
TEST(ShieldedRoot, TruncatedNullifierSetIsDetected) {
    const std::vector<uint8_t> full = {1, 2, 3, 4, 5, 6};
    const auto honest = Digest(Root32(1), 4, Acc(full), {7});
    for (size_t keep = 0; keep < full.size(); ++keep) {
        const std::vector<uint8_t> shorter(full.begin(), full.begin() + static_cast<long>(keep));
        EXPECT_NE(honest, Digest(Root32(1), 4, Acc(shorter), {7}))
            << "truncating to " << keep << " bytes went undetected";
    }
}

// Anchor-history forgery: inserting a fake anchor lets a spend prove against a
// tree state that never existed on the real chain — minting from nothing.
TEST(ShieldedRoot, InjectedAnchorChangesTheRoot) {
    const std::vector<uint8_t> anchors = {1, 2, 3, 4};
    const auto honest = Digest(Root32(1), 4, Acc({5}), anchors);
    std::vector<uint8_t> injected = anchors;
    injected.push_back(0xFF);  // one extra anchor entry
    EXPECT_NE(honest, Digest(Root32(1), 4, Acc({5}), injected));
}

// Canonical ordering is load-bearing. NullifierSet::SerializeContent() emits
// entries sorted by (height ASC, nullifier ASC); if a tampered snapshot could
// reorder them and still hash the same, the ordering guarantee would be doing
// no work. This is the property a canonical nullifier accumulator must keep.
TEST(ShieldedRoot, ReorderedNullifierContentChangesTheRoot) {
    const auto ordered = Digest(Root32(1), 4, Acc({1, 2, 3, 4}), {9});
    const auto swapped = Digest(Root32(1), 4, Acc({3, 4, 1, 2}), {9});
    EXPECT_NE(ordered, swapped);
}

// Content moved between the two sections must not cancel out — the same total
// bytes split differently is different state.
TEST(ShieldedRoot, ContentCannotBeShuffledBetweenSections) {
    const auto a = Digest(Root32(1), 4, Acc({1, 2, 3, 4}), {5, 6});
    const auto b = Digest(Root32(1), 4, Acc({1, 2, 3}), {4, 5, 6});
    const auto c = Digest(Root32(1), 4, Acc({1, 2, 3, 4, 5}), {6});
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
    const auto d = Digest(std::vector<uint8_t>(32, 0), 0, Acc({}), {});
    EXPECT_EQ(d.GetHex(), "6d08ae4f5424b20f752690ef55dee04ef8c97cedc3989284c1b283cf56f79b93");
}
