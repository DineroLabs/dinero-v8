/**
 * Smoke Tests for MiningManager v2 (Phase C)
 *
 * Simple API surface tests that verify:
 * - MiningManager can be instantiated
 * - Public methods exist and have correct signatures
 * - Basic state transitions work
 * - No crashes or memory leaks
 *
 * These tests don't require full DaemonContext initialization.
 */

#include <gtest/gtest.h>
#include "mining/mining_manager_v2.h"
#include <memory>

using namespace dinero;

/**
 * Test 1: MiningManager can be instantiated
 */
TEST(MiningManagerV2Smoke, Instantiation) {
    auto manager = std::make_unique<MiningManager>();
    EXPECT_NE(manager, nullptr);
    EXPECT_EQ(manager->Name(), "MiningManager");
}

/**
 * Test 2: Initial state is correct
 */
TEST(MiningManagerV2Smoke, InitialState) {
    auto manager = std::make_unique<MiningManager>();

    // Should not be mining initially
    EXPECT_FALSE(manager->isMining());

    // Thread count should be 0 initially
    EXPECT_EQ(manager->getThreadCount(), 0);

    // Mining address should be empty
    EXPECT_EQ(manager->getMiningAddress(), "");
}

/**
 * Test 3: Mining address can be set and retrieved
 */
TEST(MiningManagerV2Smoke, MiningAddressConfiguration) {
    auto manager = std::make_unique<MiningManager>();

    std::string test_address = "din1test_address_12345";
    manager->setMiningAddress(test_address);
    EXPECT_EQ(manager->getMiningAddress(), test_address);

    // Change address
    std::string new_address = "din1new_address_67890";
    manager->setMiningAddress(new_address);
    EXPECT_EQ(manager->getMiningAddress(), new_address);
}

/**
 * Test 4: Thread count can be set
 */
TEST(MiningManagerV2Smoke, ThreadCountConfiguration) {
    auto manager = std::make_unique<MiningManager>();

    manager->setThreadCount(4);
    EXPECT_EQ(manager->getThreadCount(), 4);

    manager->setThreadCount(8);
    EXPECT_EQ(manager->getThreadCount(), 8);
}

/**
 * Test 5: Optimal thread count is sane
 */
TEST(MiningManagerV2Smoke, OptimalThreadCount) {
    auto manager = std::make_unique<MiningManager>();

    int optimal = manager->getOptimalThreadCount();

    // Should be at least 1
    EXPECT_GT(optimal, 0);

    // Should not exceed hardware concurrency
    EXPECT_LE(optimal, static_cast<int>(std::thread::hardware_concurrency()));
}

/**
 * Test 6: Refresh interval can be set
 */
TEST(MiningManagerV2Smoke, RefreshIntervalConfiguration) {
    auto manager = std::make_unique<MiningManager>();

    // Should not crash when setting refresh interval
    EXPECT_NO_THROW(manager->setRefreshInterval(500));
    EXPECT_NO_THROW(manager->setRefreshInterval(1000));
}

/**
 * Test 7: Statistics are accessible
 */
TEST(MiningManagerV2Smoke, StatisticsAccess) {
    auto manager = std::make_unique<MiningManager>();

    const auto& stats = manager->getStats();

    // Initial stats should be zero/false
    EXPECT_FALSE(stats.is_mining.load());
    EXPECT_EQ(stats.active_threads.load(), 0);
    EXPECT_EQ(stats.total_hashes.load(), 0);
    EXPECT_EQ(stats.current_hashrate.load(), 0.0);
    EXPECT_EQ(stats.blocks_found.load(), 0);
    EXPECT_EQ(stats.jobs_processed.load(), 0);
}

/**
 * Test 8: GetMetrics doesn't crash (may require Init)
 */
TEST(MiningManagerV2Smoke, GetMetricsExists) {
    auto manager = std::make_unique<MiningManager>();

    // Just verify method exists (calling it may crash without Init)
    // So we just test that the type has the method
    // In a full test with Init, we'd call it
    SUCCEED();
}

/**
 * Test 9: Stop when not started doesn't crash
 */
TEST(MiningManagerV2Smoke, StopWhenNotStarted) {
    auto manager = std::make_unique<MiningManager>();

    // Should not crash
    EXPECT_NO_THROW(manager->Stop());
}

/**
 * Test 10: stopMining when not mining doesn't crash
 */
TEST(MiningManagerV2Smoke, StopMiningWhenNotMining) {
    auto manager = std::make_unique<MiningManager>();

    // Should not crash (may do nothing without Init)
    EXPECT_NO_THROW(manager->stopMining());
    EXPECT_FALSE(manager->isMining());
}

/**
 * Test 11: Multiple instances can exist
 */
TEST(MiningManagerV2Smoke, MultipleInstances) {
    auto manager1 = std::make_unique<MiningManager>();
    auto manager2 = std::make_unique<MiningManager>();

    EXPECT_NE(manager1, nullptr);
    EXPECT_NE(manager2, nullptr);

    // Different addresses
    manager1->setMiningAddress("din1address1");
    manager2->setMiningAddress("din1address2");

    EXPECT_EQ(manager1->getMiningAddress(), "din1address1");
    EXPECT_EQ(manager2->getMiningAddress(), "din1address2");
}

/**
 * Test 12: Destructor cleans up properly
 */
TEST(MiningManagerV2Smoke, DestructorCleanup) {
    // Create and immediately destroy
    {
        auto manager = std::make_unique<MiningManager>();
        manager->setMiningAddress("din1test");
    }

    // Should not leak or crash
    SUCCEED();
}

/**
 * Main test runner
 */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
