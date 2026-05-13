#pragma once

/**
 * Kill Switch Tester
 *
 * Tests the CT kill switch functionality during soak testing
 * by periodically engaging and disengaging it.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dinero {
namespace soak {

// Forward declarations
class SoakMetricsCollector;
class CTTxGenerator;

/**
 * Kill switch test configuration
 */
struct KillSwitchConfig {
    // Test interval
    std::chrono::hours test_interval{4};

    // Duration of each kill switch engagement
    std::chrono::minutes engagement_duration{10};

    // RPC settings for kill switch control
    std::string rpc_host = "127.0.0.1";
    int rpc_port = 20996;
    std::string rpc_user = "dinero";
    std::string rpc_password = "dinero";

    // Verification
    bool verify_mempool_clear = true;  // Verify CT txs cleared from mempool
    bool verify_no_ct_mining = true;   // Verify no CT txs mined during engagement
    int verification_attempts = 3;
    std::chrono::seconds verification_wait{5};
};

/**
 * Kill switch test result
 */
struct KillSwitchTestResult {
    bool success = false;
    std::string error;

    // Engagement metrics
    std::chrono::steady_clock::time_point engaged_at;
    std::chrono::steady_clock::time_point disengaged_at;

    // Verification results
    bool mempool_cleared = false;
    bool no_ct_mined = false;
    uint64_t ct_txs_rejected = 0;
    uint64_t blocks_during_engagement = 0;

    // Post-disengage recovery
    bool ct_resumed = false;
    std::chrono::seconds recovery_time{0};
};

/**
 * Kill switch state change callback
 */
using KillSwitchCallback = std::function<void(bool engaged, const std::string& reason)>;

/**
 * Kill Switch Tester
 *
 * Periodically engages and disengages the CT kill switch to verify
 * proper behavior during soak testing.
 */
class KillSwitchTester {
public:
    KillSwitchTester(
        const KillSwitchConfig& config,
        CTTxGenerator* tx_generator = nullptr,
        SoakMetricsCollector* metrics = nullptr
    );
    ~KillSwitchTester();

    // Non-copyable
    KillSwitchTester(const KillSwitchTester&) = delete;
    KillSwitchTester& operator=(const KillSwitchTester&) = delete;

    // Control
    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Manual testing
    KillSwitchTestResult EngageKillSwitch(const std::string& reason = "soak_test");
    KillSwitchTestResult DisengageKillSwitch();
    KillSwitchTestResult RunFullCycle();

    // State query
    bool IsKillSwitchEngaged() const { return engaged_.load(); }
    uint32_t GetTestCyclesCompleted() const { return cycles_completed_.load(); }
    uint32_t GetTestCyclesFailed() const { return cycles_failed_.load(); }

    // Callbacks
    void OnStateChange(KillSwitchCallback callback);

    // Results
    std::vector<KillSwitchTestResult> GetTestHistory() const;
    std::string GenerateReport() const;

private:
    void TesterLoop();
    bool VerifyMempoolCleared();
    bool VerifyNoCTMined(uint32_t start_height);
    bool VerifyCTResumed();
    std::string CallRPC(const std::string& method, const std::string& params);

    KillSwitchConfig config_;
    CTTxGenerator* tx_generator_;
    SoakMetricsCollector* metrics_;

    std::atomic<bool> running_{false};
    std::atomic<bool> engaged_{false};
    std::unique_ptr<std::thread> tester_thread_;

    std::atomic<uint32_t> cycles_completed_{0};
    std::atomic<uint32_t> cycles_failed_{0};

    mutable std::mutex history_mutex_;
    std::vector<KillSwitchTestResult> test_history_;

    mutable std::mutex callback_mutex_;
    std::vector<KillSwitchCallback> callbacks_;
};

} // namespace soak
} // namespace dinero
