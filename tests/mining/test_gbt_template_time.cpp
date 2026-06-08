#include "rpc/gbt_template_time.h"

#include <gtest/gtest.h>

TEST(GbtTemplateTime, UsesHeaderTimestampEvenWhenWallClockAdvances) {
    const uint64_t header_timestamp = 1780819296;
    const uint64_t later_wall_time = header_timestamp + 21;

    EXPECT_EQ(dinero::rpc::SelectGbtTemplateTime(header_timestamp, later_wall_time),
              header_timestamp);
}

