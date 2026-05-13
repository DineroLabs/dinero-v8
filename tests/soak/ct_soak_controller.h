#pragma once

/**
 * CT Soak Test Controller
 *
 * Orchestrates extended soak testing with CT transactions enabled.
 */

#include "soak_metrics.h"
#include "ct_tx_generator.h"
#include "kill_switch_tester.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dinero {
namespace soak {

/**
 * Soak test configuration
 */
struct SoakConfig {
    // Duration
    std::chrono::hours duration{24};

    // Transaction generation
    int ct_txs_per_minute = 10;
    int transparent_txs_per_minute = 50;

    // Network
    int num_nodes = 3;
    uint32_t ct_activation_height = 100;

    // Kill switch testing
    bool test_kill_switch = true;
    std::chrono::hours kill_switch_test_interval{4};
    std::chrono::minutes kill_switch_engagement_duration{10};

    // Metrics collection
    std::chrono::seconds metrics_interval{60};

    // Anomaly detection
    AnomalyThresholds anomaly_thresholds;

    // Node settings
    std::string data_dir = "/tmp/dinero-soak";
    std::vector<std::string> node_args;

    // RPC settings
    std::string rpc_user = "dinero";
    std::string rpc_password = "dinero";
    int base_rpc_port = 20996;
    int base_p2p_port = 21001;
};

/**
 * Soak test state
 */
enum class SoakState {
    NOT_STARTED,
    STARTING,
    RUNNING,
    PAUSED,
    STOPPING,
    COMPLETED,
    FAILED
};

/**
 * Soak test completion result
 */
struct SoakResult {
    bool success = false;
    SoakState final_state = SoakState::NOT_STARTED;
    std::string failure_reason;

    // Metrics summary
    SoakMetrics final_metrics;
    std::vector<AnomalyReport> anomalies;

    // Kill switch results
    uint32_t kill_switch_cycles = 0;
    uint32_t kill_switch_failures = 0;

    // Timing
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point completed_at;
    std::chrono::seconds actual_duration() const {
        return std::chrono::duration_cast<std::chrono::seconds>(completed_at - started_at);
    }
};

/**
 * Soak state change callback
 */
using SoakStateCallback = std::function<void(SoakState state, const std::string& message)>;

/**
 * CT Soak Controller
 *
 * Orchestrates the full soak test:
 * - Manages test nodes
 * - Generates continuous transactions
 * - Collects metrics
 * - Detects anomalies
 * - Tests kill switch
 */
class CTSoakController {
public:
    explicit CTSoakController(const SoakConfig& config);
    ~CTSoakController();

    // Non-copyable
    CTSoakController(const CTSoakController&) = delete;
    CTSoakController& operator=(const CTSoakController&) = delete;

    // Lifecycle
    bool Initialize();
    void Start();
    void Pause();
    void Resume();
    void Stop();
    SoakResult WaitForCompletion();

    // State
    SoakState GetState() const { return state_.load(); }
    bool IsRunning() const { return state_.load() == SoakState::RUNNING; }

    // Metrics access
    SoakMetrics GetCurrentMetrics() const;
    std::vector<AnomalyReport> GetAnomalies() const;

    // Callbacks
    void OnStateChange(SoakStateCallback callback);
    void OnAnomaly(std::function<void(const AnomalyReport&)> callback);

    // Components access (for advanced use)
    CTTxGenerator* GetTxGenerator() { return tx_generator_.get(); }
    KillSwitchTester* GetKillSwitchTester() { return kill_switch_tester_.get(); }
    SoakMetricsCollector* GetMetrics() { return metrics_.get(); }

    // Reporting
    std::string GenerateReport() const;
    bool SaveReport(const std::string& path) const;

private:
    void MainLoop();
    void CollectMetrics();
    void CheckAnomalies();
    bool StartNodes();
    void StopNodes();
    bool WaitForActivation();
    void NotifyStateChange(SoakState new_state, const std::string& message);

    SoakConfig config_;
    std::atomic<SoakState> state_{SoakState::NOT_STARTED};

    // Components
    std::unique_ptr<SoakMetricsCollector> metrics_;
    std::unique_ptr<CTTxGenerator> tx_generator_;
    std::unique_ptr<KillSwitchTester> kill_switch_tester_;

    // Node management
    std::vector<pid_t> node_pids_;

    // Thread management
    std::unique_ptr<std::thread> main_thread_;
    std::unique_ptr<std::thread> metrics_thread_;

    // Timing
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point end_time_;

    // Callbacks
    mutable std::mutex callback_mutex_;
    std::vector<SoakStateCallback> state_callbacks_;
    std::vector<std::function<void(const AnomalyReport&)>> anomaly_callbacks_;

    // Result
    SoakResult result_;
};

} // namespace soak
} // namespace dinero
