// Disk-space status classification (forest checkpoint delta campaign
// follow-up, 2026-07-17).
//
// Found on the DineroTX canary box (116 GB disk, 5.45 GB free): the FULL
// status — which hard-refuses daemon startup — triggered off the
// percentage floor (4.7% < 5%) even though the absolute free space was
// 5.4× the stated 1 GB minimum, printing the contradictory
// "Available: 5.45 GB / Minimum required: 1.0 GB / Free up disk space".
// A node that needs ~2.5 GB total (every-500 checkpoints) must be able
// to run on a mostly-full disk with ample absolute headroom: the hard
// refusal is absolute-bytes only; percentage floors demote to warnings.

#include "storage/disk_space_monitor.h"

#include <gtest/gtest.h>

using dinero::storage::ClassifyDiskSpace;
using dinero::storage::DiskLimitsConfig;
using dinero::storage::DiskSpaceStatus;

namespace {

constexpr uint64_t GB = 1024ULL * 1024 * 1024;

TEST(DiskSpaceClassify, AmpleAbsoluteSpaceOnMostlyFullDiskIsNotFull) {
    DiskLimitsConfig config;  // defaults: min 1 GB, 5% floor
    // The DineroTX shape: 116 GB disk, 5.45 GB (4.7%) available.
    const auto status = ClassifyDiskSpace(/*available=*/5583457484ULL,
                                          /*total=*/116ULL * GB, config);
    EXPECT_NE(status, DiskSpaceStatus::FULL);
}

TEST(DiskSpaceClassify, BelowAbsoluteMinimumIsFullRegardlessOfPercent) {
    DiskLimitsConfig config;
    // 0.5 GB free on a tiny 4 GB disk = 12.5% — percent is fine, bytes are not.
    EXPECT_EQ(ClassifyDiskSpace(GB / 2, 4 * GB, config), DiskSpaceStatus::FULL);
    // And on a huge disk too.
    EXPECT_EQ(ClassifyDiskSpace(GB / 2, 1000 * GB, config), DiskSpaceStatus::FULL);
}

TEST(DiskSpaceClassify, LowPercentDemotesToCriticalWarning) {
    DiskLimitsConfig config;
    // 4.7% free but 5.45 GB absolute: warn loudly, do not refuse.
    EXPECT_EQ(ClassifyDiskSpace(5583457484ULL, 116 * GB, config),
              DiskSpaceStatus::CRITICAL);
}

TEST(DiskSpaceClassify, ModestAbsoluteHeadroomIsCritical) {
    DiskLimitsConfig config;
    // 3 GB free (< min_free_bytes * 5) on a big disk.
    EXPECT_EQ(ClassifyDiskSpace(3 * GB, 500 * GB, config),
              DiskSpaceStatus::CRITICAL);
}

TEST(DiskSpaceClassify, LowSpaceThresholdWarns) {
    DiskLimitsConfig config;
    // 8 GB free on a 100 GB disk (8%): above every critical floor, below
    // the 10% low-space warning line.
    EXPECT_EQ(ClassifyDiskSpace(8 * GB, 100 * GB, config), DiskSpaceStatus::LOW);
}

TEST(DiskSpaceClassify, PlentyOfEverythingIsOk) {
    DiskLimitsConfig config;
    EXPECT_EQ(ClassifyDiskSpace(200 * GB, 500 * GB, config), DiskSpaceStatus::OK);
}

}  // namespace
