/**
 * CT Soak Controller Implementation
 */

#include "ct_soak_controller.h"

#include <csignal>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dinero {
namespace soak {

CTSoakController::CTSoakController(const SoakConfig& config)
    : config_(config)
{
}

CTSoakController::~CTSoakController() {
    Stop();
    StopNodes();
}

bool CTSoakController::Initialize() {
    if (state_.load() != SoakState::NOT_STARTED) {
        return false;
    }

    NotifyStateChange(SoakState::STARTING, "Initializing soak test");

    // Create metrics collector
    metrics_ = std::make_unique<SoakMetricsCollector>(config_.anomaly_thresholds);

    // Create transaction generator config
    TxGeneratorConfig tx_config;
    tx_config.ct_txs_per_minute = config_.ct_txs_per_minute;
    tx_config.transparent_txs_per_minute = config_.transparent_txs_per_minute;
    tx_config.rpc_port = config_.base_rpc_port;
    tx_config.rpc_user = config_.rpc_user;
    tx_config.rpc_password = config_.rpc_password;

    tx_generator_ = std::make_unique<CTTxGenerator>(tx_config, metrics_.get());

    // Create kill switch tester if enabled
    if (config_.test_kill_switch) {
        KillSwitchConfig ks_config;
        ks_config.test_interval = config_.kill_switch_test_interval;
        ks_config.engagement_duration = config_.kill_switch_engagement_duration;
        ks_config.rpc_port = config_.base_rpc_port;
        ks_config.rpc_user = config_.rpc_user;
        ks_config.rpc_password = config_.rpc_password;

        kill_switch_tester_ = std::make_unique<KillSwitchTester>(
            ks_config,
            tx_generator_.get(),
            metrics_.get()
        );
    }

    // Start test nodes
    if (!StartNodes()) {
        NotifyStateChange(SoakState::FAILED, "Failed to start test nodes");
        return false;
    }

    // Wait for CT activation height
    if (!WaitForActivation()) {
        NotifyStateChange(SoakState::FAILED, "Failed to reach CT activation height");
        StopNodes();
        return false;
    }

    return true;
}

void CTSoakController::Start() {
    if (state_.load() == SoakState::RUNNING) {
        return;
    }

    if (state_.load() == SoakState::NOT_STARTED) {
        if (!Initialize()) {
            return;
        }
    }

    NotifyStateChange(SoakState::RUNNING, "Starting soak test");
    start_time_ = std::chrono::steady_clock::now();
    result_.started_at = start_time_;

    // Start transaction generator
    tx_generator_->Start();

    // Start kill switch tester if enabled
    if (kill_switch_tester_) {
        kill_switch_tester_->Start();
    }

    // Start main loop
    main_thread_ = std::make_unique<std::thread>(&CTSoakController::MainLoop, this);

    // Start metrics collection thread
    metrics_thread_ = std::make_unique<std::thread>([this]() {
        while (state_.load() == SoakState::RUNNING || state_.load() == SoakState::PAUSED) {
            CollectMetrics();
            std::this_thread::sleep_for(config_.metrics_interval);
        }
    });
}

void CTSoakController::Pause() {
    if (state_.load() != SoakState::RUNNING) {
        return;
    }

    NotifyStateChange(SoakState::PAUSED, "Pausing soak test");
    tx_generator_->Stop();
    if (kill_switch_tester_) {
        kill_switch_tester_->Stop();
    }
}

void CTSoakController::Resume() {
    if (state_.load() != SoakState::PAUSED) {
        return;
    }

    NotifyStateChange(SoakState::RUNNING, "Resuming soak test");
    tx_generator_->Start();
    if (kill_switch_tester_) {
        kill_switch_tester_->Start();
    }
}

void CTSoakController::Stop() {
    if (state_.load() == SoakState::NOT_STARTED ||
        state_.load() == SoakState::COMPLETED ||
        state_.load() == SoakState::FAILED) {
        return;
    }

    NotifyStateChange(SoakState::STOPPING, "Stopping soak test");

    // Stop components
    if (tx_generator_) {
        tx_generator_->Stop();
    }
    if (kill_switch_tester_) {
        kill_switch_tester_->Stop();
    }

    // Wait for threads
    if (main_thread_ && main_thread_->joinable()) {
        main_thread_->join();
    }
    if (metrics_thread_ && metrics_thread_->joinable()) {
        metrics_thread_->join();
    }

    end_time_ = std::chrono::steady_clock::now();
    result_.completed_at = end_time_;
    result_.final_metrics = metrics_->GetCurrentMetrics();
    result_.anomalies = metrics_->CheckForAnomalies();

    if (kill_switch_tester_) {
        result_.kill_switch_cycles = kill_switch_tester_->GetTestCyclesCompleted();
        result_.kill_switch_failures = kill_switch_tester_->GetTestCyclesFailed();
    }

    result_.final_state = SoakState::COMPLETED;
    result_.success = !metrics_->HasCriticalAnomaly();

    NotifyStateChange(SoakState::COMPLETED, "Soak test completed");
}

SoakResult CTSoakController::WaitForCompletion() {
    if (main_thread_ && main_thread_->joinable()) {
        main_thread_->join();
    }
    if (metrics_thread_ && metrics_thread_->joinable()) {
        metrics_thread_->join();
    }
    return result_;
}

SoakMetrics CTSoakController::GetCurrentMetrics() const {
    if (metrics_) {
        return metrics_->GetCurrentMetrics();
    }
    return SoakMetrics{};
}

std::vector<AnomalyReport> CTSoakController::GetAnomalies() const {
    if (metrics_) {
        return metrics_->CheckForAnomalies();
    }
    return {};
}

void CTSoakController::OnStateChange(SoakStateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callbacks_.push_back(std::move(callback));
}

void CTSoakController::OnAnomaly(std::function<void(const AnomalyReport&)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    anomaly_callbacks_.push_back(std::move(callback));
}

std::string CTSoakController::GenerateReport() const {
    std::ostringstream oss;
    oss << "========================================\n";
    oss << "   CT Soak Test Report\n";
    oss << "========================================\n\n";

    // State
    oss << "Final State: ";
    switch (result_.final_state) {
        case SoakState::NOT_STARTED: oss << "Not Started"; break;
        case SoakState::STARTING: oss << "Starting"; break;
        case SoakState::RUNNING: oss << "Running"; break;
        case SoakState::PAUSED: oss << "Paused"; break;
        case SoakState::STOPPING: oss << "Stopping"; break;
        case SoakState::COMPLETED: oss << "Completed"; break;
        case SoakState::FAILED: oss << "Failed"; break;
    }
    oss << "\n";
    oss << "Success: " << (result_.success ? "Yes" : "No") << "\n";
    if (!result_.failure_reason.empty()) {
        oss << "Failure Reason: " << result_.failure_reason << "\n";
    }
    oss << "\n";

    // Duration
    oss << "Duration: " << result_.actual_duration().count() << " seconds\n";
    oss << "Configured Duration: " << config_.duration.count() << " hours\n\n";

    // Metrics
    if (metrics_) {
        oss << metrics_->GenerateReport();
    }

    // Kill switch results
    if (kill_switch_tester_) {
        oss << "\n" << kill_switch_tester_->GenerateReport();
    }

    return oss.str();
}

bool CTSoakController::SaveReport(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << GenerateReport();
    return true;
}

void CTSoakController::MainLoop() {
    auto target_end = start_time_ + config_.duration;

    while (state_.load() == SoakState::RUNNING || state_.load() == SoakState::PAUSED) {
        auto now = std::chrono::steady_clock::now();

        // Check if duration reached
        if (now >= target_end) {
            Stop();
            break;
        }

        // Check for anomalies
        CheckAnomalies();

        // Sleep before next check
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

void CTSoakController::CollectMetrics() {
    // TODO: Collect metrics from nodes via RPC
    // getblockcount, getmempoolinfo, getmemoryinfo, etc.

    // For now, just update the last_update timestamp
    // Real implementation would call RPC and record metrics
}

void CTSoakController::CheckAnomalies() {
    if (!metrics_) return;

    auto anomalies = metrics_->CheckForAnomalies();
    for (const auto& anomaly : anomalies) {
        // Notify callbacks
        std::lock_guard<std::mutex> lock(callback_mutex_);
        for (const auto& cb : anomaly_callbacks_) {
            cb(anomaly);
        }

        // Check for critical anomaly
        if (anomaly.metric_name == "consensus_errors") {
            result_.failure_reason = "Critical anomaly: " + anomaly.description;
            Stop();
            result_.final_state = SoakState::FAILED;
            result_.success = false;
            return;
        }
    }
}

bool CTSoakController::StartNodes() {
    // TODO: Start test nodes as separate processes
    // dinerod -datadir=<data_dir>/node<n> -rpcport=<base_rpc_port+n> etc.
    //
    // For now, this is a stub that assumes nodes are already running
    // or will be started externally

    return true;
}

void CTSoakController::StopNodes() {
#ifndef _WIN32
    for (pid_t pid : node_pids_) {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
#endif
    node_pids_.clear();
}

bool CTSoakController::WaitForActivation() {
    // TODO: Wait for chain to reach CT activation height
    // Poll getblockcount until height >= ct_activation_height

    // For now, assume already activated or activation is immediate
    return true;
}

void CTSoakController::NotifyStateChange(SoakState new_state, const std::string& message) {
    state_.store(new_state);

    std::lock_guard<std::mutex> lock(callback_mutex_);
    for (const auto& cb : state_callbacks_) {
        cb(new_state, message);
    }
}

} // namespace soak
} // namespace dinero
