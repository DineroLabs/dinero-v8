#include <gtest/gtest.h>

#include "daemon/services/header_refresh_coalescer.h"

namespace {

using dinero::daemon::HeaderRefreshAction;
using dinero::daemon::HeaderRefreshState;
using dinero::daemon::noteHeaderAnnouncement;
using dinero::daemon::takeTrailingHeaderRefresh;

constexpr auto kInterval = std::chrono::seconds(1);
const auto kStart = std::chrono::steady_clock::time_point{} + std::chrono::seconds(10);

TEST(HeaderRefreshCoalescer, BurstGetsImmediateAndExactlyOneTrailingRequest) {
    HeaderRefreshState state;

    EXPECT_EQ(noteHeaderAnnouncement(kStart, kInterval, state),
              HeaderRefreshAction::SEND_NOW);
    EXPECT_EQ(noteHeaderAnnouncement(kStart + std::chrono::milliseconds(1),
                                     kInterval, state),
              HeaderRefreshAction::QUEUE_TRAILING);
    EXPECT_EQ(noteHeaderAnnouncement(kStart + std::chrono::milliseconds(999),
                                     kInterval, state),
              HeaderRefreshAction::QUEUE_TRAILING);

    EXPECT_FALSE(takeTrailingHeaderRefresh(
        kStart + std::chrono::milliseconds(999), kInterval, state));
    EXPECT_TRUE(takeTrailingHeaderRefresh(kStart + kInterval, kInterval, state));
    EXPECT_FALSE(takeTrailingHeaderRefresh(
        kStart + kInterval + std::chrono::seconds(5), kInterval, state));
}

TEST(HeaderRefreshCoalescer, AnnouncementAfterQuietIntervalSendsImmediately) {
    HeaderRefreshState state;
    EXPECT_EQ(noteHeaderAnnouncement(kStart, kInterval, state),
              HeaderRefreshAction::SEND_NOW);
    EXPECT_EQ(noteHeaderAnnouncement(kStart + kInterval, kInterval, state),
              HeaderRefreshAction::SEND_NOW);
    EXPECT_FALSE(state.trailing_request_pending);
}

TEST(HeaderRefreshCoalescer, NewAnnouncementAfterQueuedWindowConsumesTrailingWork) {
    HeaderRefreshState state;
    EXPECT_EQ(noteHeaderAnnouncement(kStart, kInterval, state),
              HeaderRefreshAction::SEND_NOW);
    EXPECT_EQ(noteHeaderAnnouncement(kStart + std::chrono::milliseconds(10),
                                     kInterval, state),
              HeaderRefreshAction::QUEUE_TRAILING);
    EXPECT_EQ(noteHeaderAnnouncement(kStart + kInterval, kInterval, state),
              HeaderRefreshAction::SEND_NOW);
    EXPECT_FALSE(takeTrailingHeaderRefresh(
        kStart + 2 * kInterval, kInterval, state));
}

}  // namespace
