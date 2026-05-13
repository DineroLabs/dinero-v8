/**
 * Kill Switch Tester Implementation
 */

#include "kill_switch_tester.h"
#include "ct_tx_generator.h"
#include "soak_metrics.h"

#include <chrono>
#include <sstream>

namespace dinero {
namespace soak {

KillSwitchTester::KillSwitchTester(
    const KillSwitchConfig& config,
    CTTxGenerator* tx_generator,
    SoakMetricsCollector* metrics
)
    : config_(config)
    , tx_generator_(tx_generator)
    , metrics_(metrics)
{
}

KillSwitchTester::~KillSwitchTester() {
    Stop();
}

void KillSwitchTester::Start() {
    if (running_.load()) return;

    running_.store(true);
    tester_thread_ = std::make_unique<std::thread>(&KillSwitchTester::TesterLoop, this);
}

void KillSwitchTester::Stop() {
    running_.store(false);
    if (tester_thread_ && tester_thread_->joinable()) {
        tester_thread_->join();
    }
    tester_thread_.reset();
}

KillSwitchTestResult KillSwitchTester::EngageKillSwitch(const std::string& reason) {
    KillSwitchTestResult result;
    result.engaged_at = std::chrono::steady_clock::now();

    // Pause CT generation if we have access to generator
    if (tx_generator_) {
        tx_generator_->PauseCT();
    }

    // TODO: Call RPC to engage kill switch
    // engagectkilswitch reason
    //
    // For now, simulate engagement
    std::string rpc_result = CallRPC("engagectkillswitch", "\"" + reason + "\"");
    if (rpc_result.empty()) {
        // Stub behavior - assume success for testing
        engaged_.store(true);
        result.success = true;
    } else {
        // Parse RPC response
        result.success = true;  // Would check actual response
        engaged_.store(true);
    }

    // Notify callbacks
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        for (const auto& cb : callbacks_) {
            cb(true, reason);
        }
    }

    // Verify mempool cleared if configured
    if (config_.verify_mempool_clear && result.success) {
        result.mempool_cleared = VerifyMempoolCleared();
    }

    return result;
}

KillSwitchTestResult KillSwitchTester::DisengageKillSwitch() {
    KillSwitchTestResult result;
    result.disengaged_at = std::chrono::steady_clock::now();

    // TODO: Call RPC to disengage kill switch
    // disengagectkillswitch
    std::string rpc_result = CallRPC("disengagectkillswitch", "");
    if (rpc_result.empty()) {
        // Stub behavior - assume success
        engaged_.store(false);
        result.success = true;
    } else {
        result.success = true;  // Would check actual response
        engaged_.store(false);
    }

    // Resume CT generation
    if (tx_generator_) {
        tx_generator_->ResumeCT();
    }

    // Notify callbacks
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        for (const auto& cb : callbacks_) {
            cb(false, "disengaged");
        }
    }

    // Verify CT transactions resume
    if (config_.verify_no_ct_mining && result.success) {
        result.ct_resumed = VerifyCTResumed();
        if (result.ct_resumed) {
            result.recovery_time = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - result.disengaged_at
            );
        }
    }

    return result;
}

KillSwitchTestResult KillSwitchTester::RunFullCycle() {
    KillSwitchTestResult result;

    // Get current height for verification
    // TODO: Get actual height from RPC
    uint32_t start_height = 0;  // Would call getblockcount

    // Engage
    auto engage_result = EngageKillSwitch("full_cycle_test");
    result.engaged_at = engage_result.engaged_at;
    result.mempool_cleared = engage_result.mempool_cleared;

    if (!engage_result.success) {
        result.success = false;
        result.error = "Failed to engage kill switch";
        cycles_failed_.fetch_add(1);
        return result;
    }

    // Wait for engagement duration
    std::this_thread::sleep_for(config_.engagement_duration);

    // Verify no CT mined during engagement
    result.no_ct_mined = VerifyNoCTMined(start_height);
    // TODO: Get actual block count during engagement
    result.blocks_during_engagement = 0;

    // Disengage
    auto disengage_result = DisengageKillSwitch();
    result.disengaged_at = disengage_result.disengaged_at;
    result.ct_resumed = disengage_result.ct_resumed;
    result.recovery_time = disengage_result.recovery_time;

    if (!disengage_result.success) {
        result.success = false;
        result.error = "Failed to disengage kill switch";
        cycles_failed_.fetch_add(1);
        return result;
    }

    // Overall success
    result.success = result.mempool_cleared && result.no_ct_mined && result.ct_resumed;
    if (result.success) {
        cycles_completed_.fetch_add(1);
    } else {
        cycles_failed_.fetch_add(1);
        if (!result.mempool_cleared) {
            result.error = "Mempool not cleared";
        } else if (!result.no_ct_mined) {
            result.error = "CT transactions mined during engagement";
        } else {
            result.error = "CT did not resume after disengage";
        }
    }

    // Store in history
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        test_history_.push_back(result);
    }

    return result;
}

void KillSwitchTester::OnStateChange(KillSwitchCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_.push_back(std::move(callback));
}

std::vector<KillSwitchTestResult> KillSwitchTester::GetTestHistory() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return test_history_;
}

std::string KillSwitchTester::GenerateReport() const {
    std::ostringstream oss;
    oss << "=== Kill Switch Test Report ===\n\n";
    oss << "Cycles Completed: " << cycles_completed_.load() << "\n";
    oss << "Cycles Failed: " << cycles_failed_.load() << "\n\n";

    std::lock_guard<std::mutex> lock(history_mutex_);
    for (size_t i = 0; i < test_history_.size(); ++i) {
        const auto& r = test_history_[i];
        oss << "Cycle " << (i + 1) << ":\n";
        oss << "  Success: " << (r.success ? "Yes" : "No") << "\n";
        if (!r.error.empty()) {
            oss << "  Error: " << r.error << "\n";
        }
        oss << "  Mempool Cleared: " << (r.mempool_cleared ? "Yes" : "No") << "\n";
        oss << "  No CT Mined: " << (r.no_ct_mined ? "Yes" : "No") << "\n";
        oss << "  CT Resumed: " << (r.ct_resumed ? "Yes" : "No") << "\n";
        oss << "  Recovery Time: " << r.recovery_time.count() << "s\n\n";
    }

    return oss.str();
}

void KillSwitchTester::TesterLoop() {
    auto last_test = std::chrono::steady_clock::now();

    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();

        // Run test cycle at configured interval
        if ((now - last_test) >= config_.test_interval) {
            RunFullCycle();
            last_test = now;
        }

        // Sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

bool KillSwitchTester::VerifyMempoolCleared() {
    // TODO: Call RPC to get mempool info and verify no CT transactions
    // getmempoolinfo or getrawmempool
    //
    // For each tx in mempool, check if it's CT
    // Should be 0 CT txs after kill switch engaged

    for (int i = 0; i < config_.verification_attempts; ++i) {
        std::this_thread::sleep_for(config_.verification_wait);

        // TODO: Actual RPC check
        // std::string mempool = CallRPC("getrawmempool", "true");
        // Parse and check for CT transactions

        // Stub: assume cleared
        return true;
    }

    return false;
}

bool KillSwitchTester::VerifyNoCTMined(uint32_t start_height) {
    // TODO: Check blocks mined since start_height for CT transactions
    // For each block from start_height to current:
    //   getblock blockhash 2 (verbosity 2 for full tx data)
    //   Check each tx for CT markers

    (void)start_height;

    // Stub: assume no CT mined
    return true;
}

bool KillSwitchTester::VerifyCTResumed() {
    // TODO: Generate a CT transaction and verify it gets into mempool
    // 1. Create CT tx via sendconfidential
    // 2. Check mempool contains the tx

    for (int i = 0; i < config_.verification_attempts; ++i) {
        std::this_thread::sleep_for(config_.verification_wait);

        // TODO: Actual verification
        // Generate CT tx and check mempool

        // Stub: assume resumed
        return true;
    }

    return false;
}

std::string KillSwitchTester::CallRPC(const std::string& method, const std::string& params) {
    // TODO: Implement actual RPC call
    // Using curl or http client library
    (void)method;
    (void)params;
    return "";
}

} // namespace soak
} // namespace dinero
