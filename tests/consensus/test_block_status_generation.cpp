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

#include "consensus/block_status_generation.h"

using dinero::consensus::BlockStatusGeneration;
using dinero::consensus::FormatBlockStatusGeneration;
using dinero::consensus::GenerationStillCurrent;
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
