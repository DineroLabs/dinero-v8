#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <atomic>

namespace din {

/**
 * @brief Wallet metrics for observability
 * 
 * Production-ready metrics for monitoring wallet operations,
 * PSBT flows, and storage backend status.
 */
class WalletMetrics {
public:
    // PSBT operation counters
    static std::atomic<uint64_t> psbt_created_total;
    static std::atomic<uint64_t> psbt_signed_inputs_total;
    static std::atomic<uint64_t> psbt_finalized_total;
    static std::atomic<uint64_t> psbt_sign_fail_total;
    
    // Wallet state gauges
    static std::atomic<uint64_t> wallet_utxos_total;
    static std::atomic<uint64_t> wallet_balance_sats;
    static std::atomic<uint64_t> wallet_addresses_total;
    
    // Storage backend status
    static std::atomic<uint64_t> storage_backend_rocksdb;
    static std::atomic<uint64_t> storage_backend_sqlite;
    
    // Transaction builder metrics
    static std::atomic<uint64_t> tx_builder_v2_requests_total;
    static std::atomic<uint64_t> tx_builder_v2_success_total;
    static std::atomic<uint64_t> tx_builder_v2_fail_total;
    
    // Coin selection metrics
    static std::atomic<uint64_t> coin_selection_bnb_total;
    static std::atomic<uint64_t> coin_selection_greedy_total;
    static std::atomic<uint64_t> coin_selection_fail_total;
    
    /**
     * @brief Get all metrics as Prometheus format
     */
    static std::string getPrometheusMetrics();
    
    /**
     * @brief Get metrics as JSON for structured logging
     */
    static std::unordered_map<std::string, uint64_t> getMetricsSnapshot();
    
    /**
     * @brief Reset all counters (for testing)
     */
    static void reset();
    
    /**
     * @brief Increment PSBT created counter
     */
    static void incrementPsbtCreated() { psbt_created_total++; }
    
    /**
     * @brief Increment PSBT signed inputs counter
     */
    static void incrementPsbtSignedInputs(uint64_t count = 1) { 
        psbt_signed_inputs_total += count; 
    }
    
    /**
     * @brief Increment PSBT finalized counter
     */
    static void incrementPsbtFinalized() { psbt_finalized_total++; }
    
    /**
     * @brief Increment PSBT sign failure counter
     */
    static void incrementPsbtSignFail() { psbt_sign_fail_total++; }
    
    /**
     * @brief Update wallet UTXO count
     */
    static void updateUtxoCount(uint64_t count) { wallet_utxos_total = count; }
    
    /**
     * @brief Update wallet balance
     */
    static void updateBalance(uint64_t balance_sats) { wallet_balance_sats = balance_sats; }
    
    /**
     * @brief Update wallet address count
     */
    static void updateAddressCount(uint64_t count) { wallet_addresses_total = count; }
    
    /**
     * @brief Set storage backend status
     */
    static void setStorageBackend(const std::string& backend) {
        if (backend == "rocksdb") {
            storage_backend_rocksdb = 1;
            storage_backend_sqlite = 0;
        } else if (backend == "sqlite") {
            storage_backend_rocksdb = 0;
            storage_backend_sqlite = 1;
        }
    }
    
    /**
     * @brief Increment transaction builder request counter
     */
    static void incrementTxBuilderRequest() { tx_builder_v2_requests_total++; }
    
    /**
     * @brief Increment transaction builder success counter
     */
    static void incrementTxBuilderSuccess() { tx_builder_v2_success_total++; }
    
    /**
     * @brief Increment transaction builder failure counter
     */
    static void incrementTxBuilderFail() { tx_builder_v2_fail_total++; }
    
    /**
     * @brief Increment coin selection algorithm counters
     */
    static void incrementCoinSelectionBnb() { coin_selection_bnb_total++; }
    static void incrementCoinSelectionGreedy() { coin_selection_greedy_total++; }
    static void incrementCoinSelectionFail() { coin_selection_fail_total++; }
};

} // namespace din
