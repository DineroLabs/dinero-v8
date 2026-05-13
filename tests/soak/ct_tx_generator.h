#pragma once

/**
 * CT Transaction Generator
 *
 * Continuously generates confidential and transparent transactions
 * for soak testing.
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

// Forward declaration
class SoakMetricsCollector;

/**
 * Transaction generation configuration
 */
struct TxGeneratorConfig {
    // Transaction rates (per minute)
    int ct_txs_per_minute = 10;
    int transparent_txs_per_minute = 50;

    // Amount ranges (una)
    uint64_t min_amount = 10000;
    uint64_t max_amount = 100000000;

    // Fee settings (sat/vB)
    uint64_t min_fee_rate = 1;
    uint64_t max_fee_rate = 10;

    // Behavior
    bool random_amounts = true;
    bool random_fees = true;
    bool include_op_return = false;  // Include OP_RETURN outputs occasionally

    // RPC settings
    std::string rpc_host = "127.0.0.1";
    int rpc_port = 20996;
    std::string rpc_user = "dinero";
    std::string rpc_password = "dinero";
};

/**
 * Generated transaction result
 */
struct GeneratedTx {
    bool success = false;
    std::string txid;
    std::string error;
    bool is_confidential = false;
    uint64_t amount = 0;
    uint64_t fee = 0;
    std::chrono::steady_clock::time_point created_at;
};

/**
 * Transaction generation callback
 */
using TxGeneratorCallback = std::function<void(const GeneratedTx& tx)>;

/**
 * CT Transaction Generator
 *
 * Generates a continuous stream of transactions for soak testing.
 */
class CTTxGenerator {
public:
    explicit CTTxGenerator(
        const TxGeneratorConfig& config,
        SoakMetricsCollector* metrics = nullptr
    );
    ~CTTxGenerator();

    // Non-copyable
    CTTxGenerator(const CTTxGenerator&) = delete;
    CTTxGenerator& operator=(const CTTxGenerator&) = delete;

    // Control
    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Configuration
    void SetCTRate(int txs_per_minute);
    void SetTransparentRate(int txs_per_minute);
    void PauseCT();  // Pause CT generation (for kill switch testing)
    void ResumeCT();

    // Callbacks
    void OnTxGenerated(TxGeneratorCallback callback);

    // Statistics
    uint64_t GetTotalGenerated() const;
    uint64_t GetCTGenerated() const;
    uint64_t GetTransparentGenerated() const;
    uint64_t GetFailures() const;

    // Manual generation (for testing)
    GeneratedTx GenerateCTTransaction();
    GeneratedTx GenerateTransparentTransaction();

private:
    void GeneratorLoop();
    void GenerateBatch();
    std::string CallRPC(const std::string& method, const std::string& params);
    uint64_t RandomAmount();
    uint64_t RandomFeeRate();

    TxGeneratorConfig config_;
    SoakMetricsCollector* metrics_;

    std::atomic<bool> running_{false};
    std::atomic<bool> ct_paused_{false};
    std::unique_ptr<std::thread> generator_thread_;

    std::atomic<uint64_t> total_generated_{0};
    std::atomic<uint64_t> ct_generated_{0};
    std::atomic<uint64_t> transparent_generated_{0};
    std::atomic<uint64_t> failures_{0};

    mutable std::mutex callback_mutex_;
    std::vector<TxGeneratorCallback> callbacks_;
};

} // namespace soak
} // namespace dinero
