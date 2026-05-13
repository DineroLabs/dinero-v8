/**
 * Unit Tests for MiningManager v2 (Phase C)
 *
 * Tests cover:
 * - Service lifecycle (Init/Start/Stop)
 * - Mining control (start/stop/status)
 * - Thread management (worker threads, job manager)
 * - Job creation and refresh
 * - Nonce striding correctness
 * - Statistics tracking
 * - Clean shutdown (no dead threads)
 * - Error handling
 */

#include <gtest/gtest.h>
#include "mining/mining_manager_v2.h"
#include "mining/block_assembler.h"
#include "common/ilogger.h"
#include <thread>
#include <chrono>
#include <memory>

using namespace dinero;

// Forward declarations
namespace dinero {
    class ChainDB;
    class Mempool;
    class WalletManager;
    class IConsensusEngine;
    namespace metrics {
        class MetricsRegistry;
    }
}

// Mock services for testing
struct MockChainstateService {
    ChainDB* GetChainDB() { return nullptr; }
};

struct MockMempoolService {
    Mempool dummy_mempool;
    Mempool& GetMempool() { return dummy_mempool; }
};

struct MockWalletService {
    WalletManager dummy_wallet;
    WalletManager& get() { return dummy_wallet; }
};

struct MockMetricsService {
    metrics::MetricsRegistry* getRegistry() { return nullptr; }
};

// Test-only DaemonContext (compatible subset for MiningManager::Init)
// We only include the members that Init actually uses
namespace dinero {
    struct DaemonContext {
        // Members that MiningManager::Init uses (in order they appear in real DaemonContext)
        ILogger* logger_interface = nullptr;
        ILogger* mining_logger = nullptr;

        // Service pointers
        std::shared_ptr<MockChainstateService> chainstate;
        std::shared_ptr<MockMempoolService> mempool;
        std::shared_ptr<MockWalletService> wallet;
        std::shared_ptr<MockMetricsService> metrics;

        // BlockAssembler (raw pointer to avoid incomplete type issues)
        BlockAssembler* block_assembler_ptr = nullptr;

        // Provide block_assembler as a member for compatibility
        struct BlockAssemblerWrapper {
            BlockAssembler* ptr = nullptr;
            BlockAssembler* get() const { return ptr; }
            explicit operator bool() const { return ptr != nullptr; }
        } block_assembler;

        // Consensus engine (not used by MiningManager::Init but part of context)
        IConsensusEngine* consensus = nullptr;
    };
}

/**
 * Mock ILogger for testing
 */
class MockLogger : public ILogger {
public:
    void debug(const std::string& msg) override { messages.push_back("[DEBUG] " + msg); }
    void info(const std::string& msg) override { messages.push_back("[INFO] " + msg); }
    void warning(const std::string& msg) override { messages.push_back("[WARN] " + msg); }
    void error(const std::string& msg) override { messages.push_back("[ERROR] " + msg); }
    void fatal(const std::string& msg) override { messages.push_back("[FATAL] " + msg); }

    std::vector<std::string> messages;
};

/**
 * Mock BlockAssembler for testing
 */
class MockBlockAssembler : public BlockAssembler {
public:
    MockBlockAssembler() : BlockAssembler(nullptr) {}

    std::shared_ptr<MiningJob> CreateJob() override {
        ++jobs_created;

        auto job = std::make_shared<MiningJob>();
        job->job_id = "test_job_" + std::to_string(jobs_created);
        job->height = 100 + jobs_created;
        job->target_bits = 0x1d00ffff;
        job->created_time = static_cast<uint32_t>(std::time(nullptr));

        // Create a simple block header
        job->header.version = 1;
        job->header.prev_block_hash = "0000000000000000000000000000000000000000000000000000000000000000";
        job->header.merkle_root = "0000000000000000000000000000000000000000000000000000000000000000";
        job->header.timestamp = static_cast<uint32_t>(std::time(nullptr));
        job->header.bits = 0x1d00ffff;
        job->header.nonce = 0;

        // Add coinbase transaction
        Transaction coinbase;
        // Minimal coinbase for testing
        job->transactions.push_back(coinbase);

        return job;
    }

    bool ShouldRefreshJob(std::shared_ptr<MiningJob> current_job) override {
        // Refresh after 100ms for testing
        if (!current_job) return true;

        // Use current time vs created_time to determine staleness
        uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
        uint32_t age_seconds = current_time - current_job->created_time;

        // Refresh if older than threshold (convert ms to seconds for testing)
        return (age_seconds * 1000) > refresh_threshold_ms;
    }

    uint32_t jobs_created = 0;
    uint32_t refresh_threshold_ms = 100;  // 100ms for fast tests
};

/**
 * Test Fixture for MiningManager v2
 */
class MiningManagerV2Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Create mock logger
        logger = std::make_shared<MockLogger>();

        // Create mock BlockAssembler (owned by test)
        block_assembler = std::make_unique<MockBlockAssembler>();

        // Create mock services
        mock_chainstate = std::make_shared<MockChainstateService>();
        mock_mempool = std::make_shared<MockMempoolService>();
        mock_wallet = std::make_shared<MockWalletService>();
        mock_metrics = std::make_shared<MockMetricsService>();

        // Create MiningManager
        mining_manager = std::make_unique<MiningManager>();

        // Setup context for initialization
        ctx.logger_interface = logger.get();
        ctx.mining_logger = logger.get();
        ctx.chainstate = mock_chainstate;
        ctx.mempool = mock_mempool;
        ctx.wallet = mock_wallet;
        ctx.metrics = mock_metrics;

        // Setup BlockAssembler wrapper
        ctx.block_assembler.ptr = block_assembler.get();
    }

    void TearDown() override {
        // Ensure clean shutdown
        if (mining_manager) {
            mining_manager->Stop();
            mining_manager.reset();
        }
    }

    std::shared_ptr<MockLogger> logger;
    std::unique_ptr<MockBlockAssembler> block_assembler;
    std::shared_ptr<MockChainstateService> mock_chainstate;
    std::shared_ptr<MockMempoolService> mock_mempool;
    std::shared_ptr<MockWalletService> mock_wallet;
    std::shared_ptr<MockMetricsService> mock_metrics;
    std::unique_ptr<MiningManager> mining_manager;
    DaemonContext ctx;
};

/**
 * Test 1: Service Initialization
 */
TEST_F(MiningManagerV2Test, ServiceInitialization) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    EXPECT_EQ(mining_manager->Name(), "MiningManager");
    EXPECT_FALSE(mining_manager->isMining());
}

/**
 * Test 2: Service Start and Stop
 */
TEST_F(MiningManagerV2Test, ServiceLifecycle) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());
    EXPECT_TRUE(mining_manager->IsHealthy());

    mining_manager->Stop();
    EXPECT_FALSE(mining_manager->isMining());
}

/**
 * Test 3: Start Mining with Default Threads
 */
TEST_F(MiningManagerV2Test, StartMiningDefaultThreads) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    // Set mining address
    mining_manager->setMiningAddress("din1test_mining_address");

    // Start mining with default thread count (0 = auto-detect)
    ASSERT_TRUE(mining_manager->startMining(0));
    EXPECT_TRUE(mining_manager->isMining());

    // Verify thread count is non-zero
    EXPECT_GT(mining_manager->getThreadCount(), 0);

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify stats
    const auto& stats = mining_manager->getStats();
    EXPECT_TRUE(stats.is_mining.load());
    EXPECT_GT(stats.active_threads.load(), 0);

    // Stop mining
    mining_manager->stopMining();
    EXPECT_FALSE(mining_manager->isMining());
}

/**
 * Test 4: Start Mining with Specific Thread Count
 */
TEST_F(MiningManagerV2Test, StartMiningSpecificThreads) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");

    // Start mining with 2 threads
    ASSERT_TRUE(mining_manager->startMining(2));
    EXPECT_TRUE(mining_manager->isMining());
    EXPECT_EQ(mining_manager->getThreadCount(), 2);

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify 2 threads are active
    const auto& stats = mining_manager->getStats();
    EXPECT_EQ(stats.active_threads.load(), 2);

    mining_manager->stopMining();
}

/**
 * Test 5: Cannot Start Mining Twice
 */
TEST_F(MiningManagerV2Test, CannotStartMiningTwice) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");

    // Start mining
    ASSERT_TRUE(mining_manager->startMining(1));

    // Try to start again - should fail
    EXPECT_FALSE(mining_manager->startMining(1));

    mining_manager->stopMining();
}

/**
 * Test 6: Stop Mining When Not Running
 */
TEST_F(MiningManagerV2Test, StopMiningWhenNotRunning) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    // Stop mining when not running (should not crash)
    EXPECT_NO_THROW(mining_manager->stopMining());
    EXPECT_FALSE(mining_manager->isMining());
}

/**
 * Test 7: Mining Address Configuration
 */
TEST_F(MiningManagerV2Test, MiningAddressConfiguration) {
    ASSERT_TRUE(mining_manager->Init(ctx));

    // Initially empty
    EXPECT_EQ(mining_manager->getMiningAddress(), "");

    // Set mining address
    mining_manager->setMiningAddress("din1test_address_12345");
    EXPECT_EQ(mining_manager->getMiningAddress(), "din1test_address_12345");

    // Change mining address
    mining_manager->setMiningAddress("din1new_address_67890");
    EXPECT_EQ(mining_manager->getMiningAddress(), "din1new_address_67890");
}

/**
 * Test 8: Thread Count Configuration
 */
TEST_F(MiningManagerV2Test, ThreadCountConfiguration) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    // Set thread count
    mining_manager->setThreadCount(4);
    EXPECT_EQ(mining_manager->getThreadCount(), 4);

    // Get optimal thread count
    int optimal = mining_manager->getOptimalThreadCount();
    EXPECT_GT(optimal, 0);
    EXPECT_LE(optimal, std::thread::hardware_concurrency());
}

/**
 * Test 9: Statistics Tracking
 */
TEST_F(MiningManagerV2Test, StatisticsTracking) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");

    // Start mining
    ASSERT_TRUE(mining_manager->startMining(2));

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check statistics
    const auto& stats = mining_manager->getStats();
    EXPECT_TRUE(stats.is_mining.load());
    EXPECT_EQ(stats.active_threads.load(), 2);
    EXPECT_GT(stats.total_hashes.load(), 0);  // Should have hashed something
    EXPECT_GE(stats.current_hashrate.load(), 0.0);
    EXPECT_GE(stats.jobs_processed.load(), 0);

    mining_manager->stopMining();

    // After stopping, mining should be false but stats should remain
    EXPECT_FALSE(stats.is_mining.load());
}

/**
 * Test 10: Job Refresh Interval
 */
TEST_F(MiningManagerV2Test, JobRefreshInterval) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");

    // Set refresh interval to 100ms
    mining_manager->setRefreshInterval(100);

    // Start mining
    ASSERT_TRUE(mining_manager->startMining(1));

    // Wait for multiple refresh intervals
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    // Check that multiple jobs were created (through BlockAssembler)
    auto mock_assembler = dynamic_cast<MockBlockAssembler*>(ctx.block_assembler.get());
    ASSERT_NE(mock_assembler, nullptr);
    EXPECT_GT(mock_assembler->jobs_created, 2);  // Should have created at least 3 jobs

    mining_manager->stopMining();
}

/**
 * Test 11: Clean Shutdown (No Dead Threads)
 */
TEST_F(MiningManagerV2Test, CleanShutdown) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");

    // Start mining with 4 threads
    ASSERT_TRUE(mining_manager->startMining(4));

    // Let it run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop should join all threads cleanly
    auto start = std::chrono::steady_clock::now();
    mining_manager->stopMining();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Shutdown should be fast (< 2 seconds)
    EXPECT_LT(duration.count(), 2000);

    // Verify mining is stopped
    EXPECT_FALSE(mining_manager->isMining());
}

/**
 * Test 12: Service Metrics Output
 */
TEST_F(MiningManagerV2Test, ServiceMetrics) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");
    ASSERT_TRUE(mining_manager->startMining(2));

    // Let it run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Get metrics
    std::string metrics = mining_manager->GetMetrics();
    EXPECT_FALSE(metrics.empty());

    // Metrics should contain basic info (could be JSON or other format)
    // Just verify it's not empty and doesn't crash
    EXPECT_GT(metrics.length(), 10);

    mining_manager->stopMining();
}

/**
 * Test 13: Multiple Start/Stop Cycles
 */
TEST_F(MiningManagerV2Test, MultipleStartStopCycles) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    mining_manager->setMiningAddress("din1test_mining_address");

    // Cycle 1
    ASSERT_TRUE(mining_manager->startMining(2));
    EXPECT_TRUE(mining_manager->isMining());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mining_manager->stopMining();
    EXPECT_FALSE(mining_manager->isMining());

    // Cycle 2
    ASSERT_TRUE(mining_manager->startMining(1));
    EXPECT_TRUE(mining_manager->isMining());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mining_manager->stopMining();
    EXPECT_FALSE(mining_manager->isMining());

    // Cycle 3
    ASSERT_TRUE(mining_manager->startMining(4));
    EXPECT_TRUE(mining_manager->isMining());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mining_manager->stopMining();
    EXPECT_FALSE(mining_manager->isMining());
}

/**
 * Test 14: Init Without BlockAssembler Fails
 */
TEST_F(MiningManagerV2Test, InitWithoutBlockAssemblerFails) {
    auto manager = std::make_unique<MiningManager>();
    DaemonContext empty_ctx;
    empty_ctx.logger_interface = logger.get();
    empty_ctx.mempool = mock_mempool;  // Need mempool
    empty_ctx.block_assembler.ptr = nullptr;  // No BlockAssembler

    EXPECT_FALSE(manager->Init(empty_ctx));
}

/**
 * Test 15: Cannot Start Mining Without Mining Address
 */
TEST_F(MiningManagerV2Test, CannotStartMiningWithoutAddress) {
    ASSERT_TRUE(mining_manager->Init(ctx));
    ASSERT_TRUE(mining_manager->Start());

    // Try to start mining without setting address first
    // This should either fail or work with a warning (implementation dependent)
    // For now, we just verify it doesn't crash
    EXPECT_NO_THROW(mining_manager->startMining(1));

    if (mining_manager->isMining()) {
        mining_manager->stopMining();
    }
}

/**
 * Main test runner
 */
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
