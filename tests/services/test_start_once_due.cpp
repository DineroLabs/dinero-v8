// StartOnceDue (include/p2p/start_once_due.h): the rule behind starting the
// Orchard snapshot-history workers. A first-run node imports its snapshot
// minutes after P2P has started; the workers must start then, not never.
#include "p2p/start_once_due.h"

#include <gtest/gtest.h>

using dinero::p2p::StartOnceDue;

// What a fresh install does: nothing is needed at start-up, the snapshot lands
// later, and from then on the need is real.
TEST(StartOnceDue, ANeedThatAppearsAfterStartupStillStartsTheWork) {
    StartOnceDue start;
    EXPECT_FALSE(start.Due(false)) << "P2P start: no snapshot imported yet";
    EXPECT_FALSE(start.Started());
    EXPECT_FALSE(start.Due(false)) << "a later tick, still importing";
    EXPECT_TRUE(start.Due(true)) << "the snapshot is in: start the workers now";
    EXPECT_TRUE(start.Started());
}

TEST(StartOnceDue, TheWorkIsStartedExactlyOnce) {
    StartOnceDue start;
    EXPECT_TRUE(start.Due(true));
    for (int tick = 0; tick < 5; ++tick) EXPECT_FALSE(start.Due(true)) << "tick " << tick;
    EXPECT_FALSE(start.Due(false));
    EXPECT_TRUE(start.Started());
}

// A service that is stopped and started again in one process starts them again.
TEST(StartOnceDue, AResetAllowsANewStart) {
    StartOnceDue start;
    ASSERT_TRUE(start.Due(true));
    start.Reset();
    EXPECT_FALSE(start.Started());
    EXPECT_FALSE(start.Due(false));
    EXPECT_TRUE(start.Due(true));
}
