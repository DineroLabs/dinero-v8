// Copyright (c) 2026 Dinero Labs.
//
// #356: unit tests for the pure marker-guard decision that gates
// ChainstateService::ApplyStatelessReplayShielded. The decision is a header-only
// free function of two heights, so it is tested here in isolation with no
// BlockValidator / ChainDB harness. gtest's ASSERT/EXPECT macros exit non-zero
// on failure (not bare assert(), which NDEBUG would neuter), so these gate.
#include <gtest/gtest.h>

#include "daemon/services/stateless_replay_shielded_decision.h"

namespace dinero {
namespace {

using A = StatelessReplayShieldedAction;

// marker exactly at height-1 -> Apply (advance the pool by one).
TEST(StatelessReplayShieldedDecision, MarkerAtParentApplies) {
    ASSERT_EQ(StatelessReplayShieldedDecision(/*marker=*/9, /*block=*/10), A::Apply);
    ASSERT_EQ(StatelessReplayShieldedDecision(0, 1), A::Apply);
    ASSERT_EQ(StatelessReplayShieldedDecision(60999, 61000), A::Apply);
}

// marker at or ahead of the block -> Skip (already applied; a second apply
// would double-count note commitments / nullifiers).
TEST(StatelessReplayShieldedDecision, MarkerAtOrAheadSkips) {
    ASSERT_EQ(StatelessReplayShieldedDecision(/*marker=*/10, /*block=*/10), A::Skip);
    ASSERT_EQ(StatelessReplayShieldedDecision(11, 10), A::Skip);
    ASSERT_EQ(StatelessReplayShieldedDecision(1000, 10), A::Skip);
}

// marker more than one behind -> GapFail (a hole; contiguous-recovery invariant
// is broken, must fail loud rather than silently apply into a gap).
TEST(StatelessReplayShieldedDecision, MarkerBehindGapFails) {
    ASSERT_EQ(StatelessReplayShieldedDecision(/*marker=*/8, /*block=*/10), A::GapFail);
    ASSERT_EQ(StatelessReplayShieldedDecision(0, 2), A::GapFail);
    ASSERT_EQ(StatelessReplayShieldedDecision(0, 61000), A::GapFail);
}

// Boundary: block height 0 (genesis). block-1 would underflow if evaluated;
// the ordering guarantees Skip for every marker (genesis carries no shielded
// activity to replay) and never touches the subtraction.
TEST(StatelessReplayShieldedDecision, GenesisHeightSkipsWithoutUnderflow) {
    ASSERT_EQ(StatelessReplayShieldedDecision(/*marker=*/0, /*block=*/0), A::Skip);
    ASSERT_EQ(StatelessReplayShieldedDecision(5, 0), A::Skip);
    // Max marker against genesis must still be Skip, never a spurious Apply from
    // an underflowed (block-1 == UINT32_MAX) comparison.
    ASSERT_EQ(StatelessReplayShieldedDecision(0xFFFFFFFFu, 0), A::Skip);
}

}  // namespace
}  // namespace dinero
