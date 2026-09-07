// Copyright (c) 2026 Dinero Labs.
//
// Losing a benign single-flight race is NEITHER success NOR peer misconduct.
//
// The old code used BlockRejectCode::DUPLICATE for it, and the two consumers
// then wanted opposite wrong things:
//
//   * the scheduler drain read it as SUCCESS -- marked the block CONNECTED and
//     advanced local_tip_height_ for a block whose real outcome was unknown and
//     which might never connect;
//   * the relay manager read it as FAILURE -- bumped blocks_rejected and called
//     RecordBlockFailure against an honest peer that had done nothing wrong.
//
// That they disagreed is the proof the outcome is neither. These tests pin the
// non-terminal contract on both sides.

#include <gtest/gtest.h>

#include "daemon/block_relay_manager.h"
#include "daemon/interfaces/ingress_types.h"

using dinero::BlockRelayManager;
using Outcome = BlockRelayManager::BlockValidationOutcome;

TEST(ConcurrentInFlight, IsDistinctFromBothAcceptedAndRejected) {
    EXPECT_TRUE(Outcome(Outcome::ConcurrentInFlight) == Outcome::ConcurrentInFlight);
    EXPECT_TRUE(Outcome(Outcome::ConcurrentInFlight) != Outcome::Accepted)
        << "treating it as success advanced a tip for a block that may never connect";
    EXPECT_TRUE(Outcome(Outcome::ConcurrentInFlight) != Outcome::Rejected)
        << "treating it as failure smeared an honest peer's reputation";
}

// The bool conversion must keep meaning exactly what it always meant, so the
// existing validators are unaffected and only the new state is new.
TEST(ConcurrentInFlight, BoolStillMeansAcceptedOrRejected) {
    EXPECT_TRUE(Outcome(true) == Outcome::Accepted);
    EXPECT_TRUE(Outcome(false) == Outcome::Rejected);
    EXPECT_TRUE(Outcome(false) != Outcome::ConcurrentInFlight)
        << "a plain false must never silently become the non-terminal state";
}

// The reject code carries its own name. Reusing DUPLICATE asserted the body was
// already known, which nothing on this path has established -- the winner may
// be performing the very first write, or may fail and never write.
TEST(ConcurrentInFlight, RejectCodeIsNotDuplicate) {
    EXPECT_NE(static_cast<int>(dinero::BlockRejectCode::CONCURRENT_IN_FLIGHT),
              static_cast<int>(dinero::BlockRejectCode::DUPLICATE));
    EXPECT_STREQ(dinero::BlockRejectCodeToString(
                     dinero::BlockRejectCode::CONCURRENT_IN_FLIGHT),
                 "concurrent-in-flight");
}
