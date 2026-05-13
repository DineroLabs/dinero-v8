#include "wallet/wallet_metrics.h"
#include <sstream>
#include <iomanip>

namespace din {

// Initialize static atomic counters
std::atomic<uint64_t> WalletMetrics::psbt_created_total{0};
std::atomic<uint64_t> WalletMetrics::psbt_signed_inputs_total{0};
std::atomic<uint64_t> WalletMetrics::psbt_finalized_total{0};
std::atomic<uint64_t> WalletMetrics::psbt_sign_fail_total{0};

std::atomic<uint64_t> WalletMetrics::wallet_utxos_total{0};
std::atomic<uint64_t> WalletMetrics::wallet_balance_sats{0};
std::atomic<uint64_t> WalletMetrics::wallet_addresses_total{0};

std::atomic<uint64_t> WalletMetrics::storage_backend_rocksdb{1}; // Default to RocksDB
std::atomic<uint64_t> WalletMetrics::storage_backend_sqlite{0};

std::atomic<uint64_t> WalletMetrics::tx_builder_v2_requests_total{0};
std::atomic<uint64_t> WalletMetrics::tx_builder_v2_success_total{0};
std::atomic<uint64_t> WalletMetrics::tx_builder_v2_fail_total{0};

std::atomic<uint64_t> WalletMetrics::coin_selection_bnb_total{0};
std::atomic<uint64_t> WalletMetrics::coin_selection_greedy_total{0};
std::atomic<uint64_t> WalletMetrics::coin_selection_fail_total{0};

std::string WalletMetrics::getPrometheusMetrics() {
    std::ostringstream oss;
    
    // PSBT metrics
    oss << "# HELP wallet_psbt_created_total Total PSBTs created\n";
    oss << "# TYPE wallet_psbt_created_total counter\n";
    oss << "wallet_psbt_created_total " << psbt_created_total.load() << "\n\n";
    
    oss << "# HELP wallet_psbt_signed_inputs_total Total PSBT inputs signed\n";
    oss << "# TYPE wallet_psbt_signed_inputs_total counter\n";
    oss << "wallet_psbt_signed_inputs_total " << psbt_signed_inputs_total.load() << "\n\n";
    
    oss << "# HELP wallet_psbt_finalized_total Total PSBTs finalized\n";
    oss << "# TYPE wallet_psbt_finalized_total counter\n";
    oss << "wallet_psbt_finalized_total " << psbt_finalized_total.load() << "\n\n";
    
    oss << "# HELP wallet_psbt_sign_fail_total Total PSBT sign failures\n";
    oss << "# TYPE wallet_psbt_sign_fail_total counter\n";
    oss << "wallet_psbt_sign_fail_total " << psbt_sign_fail_total.load() << "\n\n";
    
    // Wallet state metrics
    oss << "# HELP wallet_utxos_total Total UTXOs in wallet\n";
    oss << "# TYPE wallet_utxos_total gauge\n";
    oss << "wallet_utxos_total " << wallet_utxos_total.load() << "\n\n";
    
    oss << "# HELP wallet_balance_sats Total wallet balance in una\n";
    oss << "# TYPE wallet_balance_sats gauge\n";
    oss << "wallet_balance_sats " << wallet_balance_sats.load() << "\n\n";
    
    oss << "# HELP wallet_addresses_total Total addresses in wallet\n";
    oss << "# TYPE wallet_addresses_total gauge\n";
    oss << "wallet_addresses_total " << wallet_addresses_total.load() << "\n\n";
    
    // Storage backend metrics
    oss << "# HELP dinerod_storage_backend Storage backend in use\n";
    oss << "# TYPE dinerod_storage_backend gauge\n";
    oss << "dinerod_storage_backend{backend=\"rocksdb\"} " << storage_backend_rocksdb.load() << "\n";
    oss << "dinerod_storage_backend{backend=\"sqlite\"} " << storage_backend_sqlite.load() << "\n\n";
    
    // Transaction builder metrics
    oss << "# HELP wallet_tx_builder_v2_requests_total Total V2 transaction builder requests\n";
    oss << "# TYPE wallet_tx_builder_v2_requests_total counter\n";
    oss << "wallet_tx_builder_v2_requests_total " << tx_builder_v2_requests_total.load() << "\n\n";
    
    oss << "# HELP wallet_tx_builder_v2_success_total Total V2 transaction builder successes\n";
    oss << "# TYPE wallet_tx_builder_v2_success_total counter\n";
    oss << "wallet_tx_builder_v2_success_total " << tx_builder_v2_success_total.load() << "\n\n";
    
    oss << "# HELP wallet_tx_builder_v2_fail_total Total V2 transaction builder failures\n";
    oss << "# TYPE wallet_tx_builder_v2_fail_total counter\n";
    oss << "wallet_tx_builder_v2_fail_total " << tx_builder_v2_fail_total.load() << "\n\n";
    
    // Coin selection metrics
    oss << "# HELP wallet_coin_selection_bnb_total Total Branch-and-Bound coin selections\n";
    oss << "# TYPE wallet_coin_selection_bnb_total counter\n";
    oss << "wallet_coin_selection_bnb_total " << coin_selection_bnb_total.load() << "\n\n";
    
    oss << "# HELP wallet_coin_selection_greedy_total Total greedy coin selections\n";
    oss << "# TYPE wallet_coin_selection_greedy_total counter\n";
    oss << "wallet_coin_selection_greedy_total " << coin_selection_greedy_total.load() << "\n\n";
    
    oss << "# HELP wallet_coin_selection_fail_total Total coin selection failures\n";
    oss << "# TYPE wallet_coin_selection_fail_total counter\n";
    oss << "wallet_coin_selection_fail_total " << coin_selection_fail_total.load() << "\n\n";
    
    return oss.str();
}

std::unordered_map<std::string, uint64_t> WalletMetrics::getMetricsSnapshot() {
    std::unordered_map<std::string, uint64_t> snapshot;
    
    snapshot["psbt_created_total"] = psbt_created_total.load();
    snapshot["psbt_signed_inputs_total"] = psbt_signed_inputs_total.load();
    snapshot["psbt_finalized_total"] = psbt_finalized_total.load();
    snapshot["psbt_sign_fail_total"] = psbt_sign_fail_total.load();
    
    snapshot["wallet_utxos_total"] = wallet_utxos_total.load();
    snapshot["wallet_balance_sats"] = wallet_balance_sats.load();
    snapshot["wallet_addresses_total"] = wallet_addresses_total.load();
    
    snapshot["storage_backend_rocksdb"] = storage_backend_rocksdb.load();
    snapshot["storage_backend_sqlite"] = storage_backend_sqlite.load();
    
    snapshot["tx_builder_v2_requests_total"] = tx_builder_v2_requests_total.load();
    snapshot["tx_builder_v2_success_total"] = tx_builder_v2_success_total.load();
    snapshot["tx_builder_v2_fail_total"] = tx_builder_v2_fail_total.load();
    
    snapshot["coin_selection_bnb_total"] = coin_selection_bnb_total.load();
    snapshot["coin_selection_greedy_total"] = coin_selection_greedy_total.load();
    snapshot["coin_selection_fail_total"] = coin_selection_fail_total.load();
    
    return snapshot;
}

void WalletMetrics::reset() {
    psbt_created_total = 0;
    psbt_signed_inputs_total = 0;
    psbt_finalized_total = 0;
    psbt_sign_fail_total = 0;
    
    wallet_utxos_total = 0;
    wallet_balance_sats = 0;
    wallet_addresses_total = 0;
    
    tx_builder_v2_requests_total = 0;
    tx_builder_v2_success_total = 0;
    tx_builder_v2_fail_total = 0;
    
    coin_selection_bnb_total = 0;
    coin_selection_greedy_total = 0;
    coin_selection_fail_total = 0;
}

} // namespace din
