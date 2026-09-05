// Generation-checked block status transitions.
//
// The rule: a stale block-processing result may never overwrite a NEWER
// operator decision.
//
// The bug this fixes, measured on a live node: BlockAcceptor preserves a
// block's BLOCK_FAILED_VALID when a peer re-relays a previously-invalidated
// block (Bug #6/#38) so a relay cannot silently undo an invalidation. Correct
// alone — but a continuously re-announced block always has a relay in flight,
// so `reconsiderblock` was undone by the very next one. Observed: 650
// re-assertions AFTER the reconsider, tip never recovered, invalidation
// effectively irreversible.
#include <gtest/gtest.h>

#include <limits>

#include "consensus/block_status_generation.h"

using dinero::consensus::BlockStatusGeneration;
using dinero::consensus::FormatBlockStatusGeneration;
using dinero::consensus::GenerationStillCurrent;
using dinero::consensus::NextGeneration;
using dinero::consensus::PreservedFailureFlags;
using dinero::consensus::CompareThenCommitFailureFlags;
using dinero::consensus::ParseBlockStatusGeneration;

TEST(BlockStatusGeneration, ResultCapturedBeforeADecisionIsStale) {
    // A relay that began at generation 7 and commits after a reconsider bumped
    // to 8 must NOT be allowed to preserve or assert failure flags.
    EXPECT_FALSE(GenerationStillCurrent(7, 8))
        << "a pre-reconsider acceptance must not restore BLOCK_FAILED_VALID";
}

TEST(BlockStatusGeneration, ResultCapturedAfterADecisionIsCurrent) {
    // A block genuinely validated as invalid AFTER the reconsider captured the
    // new generation, so it may still set failure state normally. The rule
    // must not disarm legitimate invalidation.
    EXPECT_TRUE(GenerationStillCurrent(8, 8))
        << "a post-reconsider validation must still be able to set failure state";
}

TEST(BlockStatusGeneration, AnyAdvanceInvalidates) {
    for (BlockStatusGeneration captured = 0; captured < 5; ++captured) {
        for (BlockStatusGeneration now = 0; now < 5; ++now) {
            EXPECT_EQ(GenerationStillCurrent(captured, now), captured == now)
                << "captured=" << captured << " now=" << now;
        }
    }
}

TEST(BlockStatusGeneration, UnreadableCounterFailsClosed) {
    // A malformed or absent counter must read as 0, which never equals a
    // bumped generation — so the result is treated as STALE and the flags are
    // dropped, rather than a wrong value making a stale result look current.
    for (const char* junk : {"", "abc", "12x", " 4", "-1", "0x10", "99999999999999999999999999"}) {
        EXPECT_EQ(ParseBlockStatusGeneration(junk), 0u) << "input: '" << junk << "'";
    }
    // And a 0 read is never current against any real decision.
    EXPECT_FALSE(GenerationStillCurrent(ParseBlockStatusGeneration("garbage"), 1));
}

TEST(BlockStatusGeneration, RoundTripsThroughPersistence) {
    for (BlockStatusGeneration g : {uint64_t{1}, uint64_t{2}, uint64_t{650},
                                    uint64_t{4294967296}, uint64_t{18446744073709551615ull}}) {
        EXPECT_EQ(ParseBlockStatusGeneration(FormatBlockStatusGeneration(g)), g);
    }
}

TEST(BlockStatusGeneration, ZeroIsDistinguishableFromARealDecision) {
    // Generation 0 means "never recorded". Any real invalidate or reconsider
    // produces >= 1, so a captured 0 cannot be confused with a genuine early
    // generation and cannot make a stale result look authoritative.
    EXPECT_TRUE(GenerationStillCurrent(0, 0));
    EXPECT_FALSE(GenerationStillCurrent(0, 1));
    EXPECT_FALSE(GenerationStillCurrent(1, 0));
}

TEST(BlockStatusGeneration, MalformedValueCannotCompareEqualToAZeroExpectation) {
    // The dangerous corner: if the EXPECTED generation is 0 (never recorded,
    // or itself unreadable) and a malformed persisted value also decodes to 0,
    // a naive comparison would say "unchanged" and let a stale result through.
    //
    // Fail-closed only works if 0 is treated as "no valid decision recorded",
    // never as a generation that can match. Every real invalidate/reconsider
    // produces >= 1, so a 0-vs-0 comparison can only arise when NOTHING has
    // been decided — in which case there is no operator decision to protect
    // and preserving flags is harmless.
    const auto junk = ParseBlockStatusGeneration("not-a-number");
    ASSERT_EQ(junk, 0u);
    // 0 == 0 is structurally "current", and that is safe ONLY because no
    // decision exists at generation 0. Pin the reasoning so a future change
    // that starts real generations at 0 has to confront it.
    EXPECT_TRUE(GenerationStillCurrent(junk, 0))
        << "0 vs 0 means no decision has ever been recorded";
    // The moment ANY decision exists, a malformed read can never match it.
    for (BlockStatusGeneration real = 1; real < 6; ++real) {
        EXPECT_FALSE(GenerationStillCurrent(junk, real))
            << "a malformed read must never equal real generation " << real;
        EXPECT_FALSE(GenerationStillCurrent(real, junk))
            << "a real capture must never equal a malformed current value";
    }
}

TEST(BlockStatusGeneration, SaturationNeverWrapsToZero) {
    // Wrapping would be the worst outcome: every in-flight result would read
    // stale forever, and a later wrap could make a genuinely stale capture
    // compare EQUAL to the current value. The counter saturates instead.
    constexpr auto kMax = std::numeric_limits<BlockStatusGeneration>::max();
    EXPECT_TRUE(GenerationStillCurrent(kMax, kMax));
    EXPECT_FALSE(GenerationStillCurrent(kMax, 0))
        << "if the counter ever wrapped to 0, a max-generation capture must "
           "still read as stale";
    EXPECT_EQ(ParseBlockStatusGeneration(FormatBlockStatusGeneration(kMax)), kMax);
}

TEST(BlockStatusGeneration, OverflowRefusesAndLeavesTheCounterUnchanged) {
    // The refusal must return "no next value" so the caller leaves BOTH the
    // generation and the failure flags untouched — a partial application here
    // would be worse than the overflow.
    constexpr auto kMax = std::numeric_limits<BlockStatusGeneration>::max();
    EXPECT_FALSE(NextGeneration(kMax).has_value())
        << "a saturated counter must refuse, never wrap to 0";

    // Everything below saturation advances by exactly one.
    for (BlockStatusGeneration g : {uint64_t{0}, uint64_t{1}, uint64_t{650},
                                    uint64_t{kMax - 2}, uint64_t{kMax - 1}}) {
        const auto n = NextGeneration(g);
        ASSERT_TRUE(n.has_value()) << "g=" << g;
        EXPECT_EQ(*n, g + 1) << "g=" << g;
        EXPECT_NE(*n, 0u) << "an advance must never produce 0";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TOCTOU between the generation comparison and the failure-flag commit.
//
// This is deliberately a LOW-LEVEL test with a fake store. A two-daemon
// fixture could park ConnectBlock at the exact boundary, but removing
// ReconsiderBlock's activation_mutex_ still produced no observable difference
// there: the stale path only does damage when it is carrying non-zero flags
// AND its metadata write is the last writer. ActivateBestChain, relay handling
// and candidate processing all sit in between and mask it, so the fixture
// tested too much surrounding machinery to isolate one lock.
//
// Here the payload is guaranteed: a captured FAILED flag, an old generation,
// and a commit that nothing else can overwrite.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr uint32_t kFailedValid = 1u << 0;   // stands in for BLOCK_FAILED_VALID

/// Minimal fake of the persistent decision state.
struct FakeStore {
    BlockStatusGeneration generation = 7;
    uint32_t flags = kFailedValid;
    uint32_t committed = 0xFFFFFFFF;   // sentinel: nothing committed yet

    /// What ReconsiderBlock does: clear the flags and advance the generation.
    void Reconsider() {
        flags = 0;
        generation = *NextGeneration(generation);
    }
};
}  // namespace

TEST(GenerationToctou, UnsynchronizedWindowLetsAStaleResultReassertClearedFlags) {
    // THE HAZARD. Acceptance compares at generation 7, a reconsider lands in
    // the window, and the stale commit carries the old flag forward.
    FakeStore store;
    const auto captured = store.generation;            // 7
    const uint32_t observed = store.flags;             // FAILED_VALID
    ASSERT_NE(observed, 0u) << "the fixture must supply a damaging payload";

    CompareThenCommitFailureFlags(
        captured,
        [&] { return store.generation; },
        observed,
        [&] { store.Reconsider(); },                   // window: unsynchronized
        [&](uint32_t f) { store.committed = f; });

    EXPECT_EQ(store.generation, 8u) << "the reconsider did advance the generation";
    EXPECT_EQ(store.flags, 0u) << "the reconsider did clear the flags";
    EXPECT_EQ(store.committed, kFailedValid)
        << "UNSYNCHRONIZED: the stale commit re-asserted a flag the operator "
           "had just cleared — this is the bug activation_mutex_ prevents";
}

TEST(GenerationToctou, SerializedWindowCommitsNothingStale) {
    // WITH serialization the reconsider cannot run inside the window, so the
    // commit reflects the state the comparison actually saw.
    FakeStore store;
    const auto captured = store.generation;
    const uint32_t observed = store.flags;

    CompareThenCommitFailureFlags(
        captured,
        [&] { return store.generation; },
        observed,
        [] { /* serialized: nothing may run here */ },
        [&](uint32_t f) { store.committed = f; });

    EXPECT_EQ(store.committed, kFailedValid)
        << "no decision intervened, so preserving the observed flag is correct";

    // And once the reconsider DOES run — after the sequence, as the lock
    // guarantees — a subsequent acceptance sees the new generation and carries
    // nothing forward.
    store.Reconsider();
    const auto after = store.generation;
    EXPECT_EQ(PreservedFailureFlags(captured, after, kFailedValid), 0u)
        << "a result captured before the reconsider must carry nothing forward";
}

TEST(GenerationToctou, RecapturingAfterTheDecisionIsSafe) {
    // The ordering the mutex enforces: capture, compare and commit all sit on
    // one side of the decision. A result that captures AFTER the reconsider is
    // current and may legitimately record failure again.
    FakeStore store;
    store.Reconsider();                       // generation 8, flags cleared
    const auto captured = store.generation;   // captured AFTER
    store.flags = kFailedValid;               // genuinely invalid this time

    CompareThenCommitFailureFlags(
        captured,
        [&] { return store.generation; },
        store.flags,
        [] {},
        [&](uint32_t f) { store.committed = f; });

    EXPECT_EQ(store.committed, kFailedValid)
        << "a post-decision validation must still be able to record failure";
}
