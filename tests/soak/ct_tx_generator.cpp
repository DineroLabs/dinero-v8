/**
 * CT Transaction Generator Implementation
 */

#include "ct_tx_generator.h"
#include "soak_metrics.h"

#include <chrono>
#include <random>

namespace dinero {
namespace soak {

CTTxGenerator::CTTxGenerator(
    const TxGeneratorConfig& config,
    SoakMetricsCollector* metrics
)
    : config_(config)
    , metrics_(metrics)
{
}

CTTxGenerator::~CTTxGenerator() {
    Stop();
}

void CTTxGenerator::Start() {
    if (running_.load()) return;

    running_.store(true);
    generator_thread_ = std::make_unique<std::thread>(&CTTxGenerator::GeneratorLoop, this);
}

void CTTxGenerator::Stop() {
    running_.store(false);
    if (generator_thread_ && generator_thread_->joinable()) {
        generator_thread_->join();
    }
    generator_thread_.reset();
}

void CTTxGenerator::SetCTRate(int txs_per_minute) {
    config_.ct_txs_per_minute = txs_per_minute;
}

void CTTxGenerator::SetTransparentRate(int txs_per_minute) {
    config_.transparent_txs_per_minute = txs_per_minute;
}

void CTTxGenerator::PauseCT() {
    ct_paused_.store(true);
}

void CTTxGenerator::ResumeCT() {
    ct_paused_.store(false);
}

void CTTxGenerator::OnTxGenerated(TxGeneratorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_.push_back(std::move(callback));
}

uint64_t CTTxGenerator::GetTotalGenerated() const {
    return total_generated_.load();
}

uint64_t CTTxGenerator::GetCTGenerated() const {
    return ct_generated_.load();
}

uint64_t CTTxGenerator::GetTransparentGenerated() const {
    return transparent_generated_.load();
}

uint64_t CTTxGenerator::GetFailures() const {
    return failures_.load();
}

GeneratedTx CTTxGenerator::GenerateCTTransaction() {
    GeneratedTx tx;
    tx.is_confidential = true;
    tx.created_at = std::chrono::steady_clock::now();
    tx.amount = RandomAmount();

    // TODO: Integrate with actual RPC
    // For now, this is a stub that simulates generation
    //
    // Real implementation would:
    // 1. Call RPC to get a new CT address: getnewaddress "" confidential
    // 2. Call sendtoaddress or sendconfidential
    // 3. Record the txid

    // Simulate success (real impl would call RPC)
    tx.success = false;
    tx.error = "CT transaction generation requires RPC integration";

    if (tx.success) {
        ct_generated_.fetch_add(1);
        total_generated_.fetch_add(1);
        if (metrics_) {
            metrics_->RecordCTTransaction(false);  // Not yet confirmed
        }
    } else {
        failures_.fetch_add(1);
    }

    // Notify callbacks
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        for (const auto& cb : callbacks_) {
            cb(tx);
        }
    }

    return tx;
}

GeneratedTx CTTxGenerator::GenerateTransparentTransaction() {
    GeneratedTx tx;
    tx.is_confidential = false;
    tx.created_at = std::chrono::steady_clock::now();
    tx.amount = RandomAmount();

    // TODO: Integrate with actual RPC
    // For now, this is a stub
    //
    // Real implementation would:
    // 1. Call getnewaddress
    // 2. Call sendtoaddress

    tx.success = false;
    tx.error = "Transparent transaction generation requires RPC integration";

    if (tx.success) {
        transparent_generated_.fetch_add(1);
        total_generated_.fetch_add(1);
        if (metrics_) {
            metrics_->RecordTransparentTransaction(false);
        }
    } else {
        failures_.fetch_add(1);
    }

    // Notify callbacks
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        for (const auto& cb : callbacks_) {
            cb(tx);
        }
    }

    return tx;
}

void CTTxGenerator::GeneratorLoop() {
    // Calculate delays based on rates
    auto ct_delay = std::chrono::milliseconds(
        config_.ct_txs_per_minute > 0 ? (60000 / config_.ct_txs_per_minute) : 60000
    );
    auto transparent_delay = std::chrono::milliseconds(
        config_.transparent_txs_per_minute > 0 ? (60000 / config_.transparent_txs_per_minute) : 60000
    );

    auto last_ct = std::chrono::steady_clock::now();
    auto last_transparent = std::chrono::steady_clock::now();

    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();

        // Generate CT transaction if not paused and delay elapsed
        if (!ct_paused_.load() && (now - last_ct) >= ct_delay) {
            GenerateCTTransaction();
            last_ct = now;
        }

        // Generate transparent transaction if delay elapsed
        if ((now - last_transparent) >= transparent_delay) {
            GenerateTransparentTransaction();
            last_transparent = now;
        }

        // Sleep briefly to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void CTTxGenerator::GenerateBatch() {
    // Generate a batch of transactions at once
    // Useful for burst testing
    if (!ct_paused_.load()) {
        GenerateCTTransaction();
    }
    GenerateTransparentTransaction();
}

std::string CTTxGenerator::CallRPC(const std::string& method, const std::string& params) {
    // TODO: Implement actual RPC call using curl or http client
    // For now, return empty indicating not implemented
    (void)method;
    (void)params;
    return "";
}

uint64_t CTTxGenerator::RandomAmount() {
    if (!config_.random_amounts) {
        return (config_.min_amount + config_.max_amount) / 2;
    }

    static thread_local std::mt19937_64 gen(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(config_.min_amount, config_.max_amount);
    return dist(gen);
}

uint64_t CTTxGenerator::RandomFeeRate() {
    if (!config_.random_fees) {
        return (config_.min_fee_rate + config_.max_fee_rate) / 2;
    }

    static thread_local std::mt19937_64 gen(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(config_.min_fee_rate, config_.max_fee_rate);
    return dist(gen);
}

} // namespace soak
} // namespace dinero
