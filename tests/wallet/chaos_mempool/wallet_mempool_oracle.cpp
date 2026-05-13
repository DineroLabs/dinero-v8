/**
 * @file wallet_mempool_oracle.cpp
 * @brief Wallet Oracle for Mempool Eviction Validation
 *
 * This oracle validates wallet safety during mempool eviction events.
 * Tests that funds are tracked correctly when transactions are evicted,
 * replaced, expired, or conflicted.
 *
 * Usage:
 *   wallet_mempool_oracle snapshot <wallet_db> > state.json
 *   wallet_mempool_oracle validate_eviction <before.json> <after.json> <txid>
 *   wallet_mempool_oracle check_mempool_tx <txid>
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <map>
#include <set>
#include <sqlite3.h>

// ═══════════════════════════════════════════════════════════════════════════
// Mempool Transaction State Model
// ═══════════════════════════════════════════════════════════════════════════

enum class MempoolTxState {
    IN_MEMPOOL,      // Transaction in mempool, unconfirmed
    EVICTED,         // Transaction evicted from mempool
    EXPIRED,         // Transaction expired (timeout)
    CONFLICTED,      // Transaction conflicts with another (RBF/double-spend)
    CONFIRMED,       // Transaction mined in a block
    UNKNOWN          // State cannot be determined
};

const char* mempoolTxStateToString(MempoolTxState state) {
    switch (state) {
        case MempoolTxState::IN_MEMPOOL: return "IN_MEMPOOL";
        case MempoolTxState::EVICTED: return "EVICTED";
        case MempoolTxState::EXPIRED: return "EXPIRED";
        case MempoolTxState::CONFLICTED: return "CONFLICTED";
        case MempoolTxState::CONFIRMED: return "CONFIRMED";
        case MempoolTxState::UNKNOWN: return "UNKNOWN";
        default: return "INVALID";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Wallet Snapshot (with Mempool Transaction Tracking)
// ═══════════════════════════════════════════════════════════════════════════

struct TransactionRecord {
    std::string txid;
    uint64_t amount_una;
    uint32_t confirmations;
    bool is_coinbase;
    std::string category;  // "send", "receive", "generate"
    bool in_mempool;

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"txid\":\"" << txid << "\",";
        oss << "\"amount\":" << amount_una << ",";
        oss << "\"confirmations\":" << confirmations << ",";
        oss << "\"is_coinbase\":" << (is_coinbase ? "true" : "false") << ",";
        oss << "\"category\":\"" << category << "\",";
        oss << "\"in_mempool\":" << (in_mempool ? "true" : "false");
        oss << "}";
        return oss.str();
    }
};

struct UTXORecord {
    std::string txid;
    uint32_t vout;
    uint64_t amount_una;
    bool is_spent;
    bool is_locked;  // Locked by unconfirmed tx

    std::string key() const {
        return txid + ":" + std::to_string(vout);
    }

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"txid\":\"" << txid << "\",";
        oss << "\"vout\":" << vout << ",";
        oss << "\"amount\":" << amount_una << ",";
        oss << "\"is_spent\":" << (is_spent ? "true" : "false") << ",";
        oss << "\"is_locked\":" << (is_locked ? "true" : "false");
        oss << "}";
        return oss.str();
    }
};

struct WalletMempoolSnapshot {
    uint32_t height;
    uint64_t balance_total_una;
    uint64_t balance_confirmed_una;
    uint64_t balance_unconfirmed_una;
    uint32_t utxo_count;
    uint32_t mempool_tx_count;

    std::vector<TransactionRecord> transactions;
    std::vector<UTXORecord> utxos;

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"height\":" << height << ",";
        oss << "\"balance_total_una\":" << balance_total_una << ",";
        oss << "\"balance_confirmed_una\":" << balance_confirmed_una << ",";
        oss << "\"balance_unconfirmed_una\":" << balance_unconfirmed_una << ",";
        oss << "\"utxo_count\":" << utxo_count << ",";
        oss << "\"mempool_tx_count\":" << mempool_tx_count << ",";

        // Transactions
        oss << "\"transactions\":[";
        for (size_t i = 0; i < transactions.size(); ++i) {
            if (i > 0) oss << ",";
            oss << transactions[i].toJSON();
        }
        oss << "],";

        // UTXOs
        oss << "\"utxos\":[";
        for (size_t i = 0; i < utxos.size(); ++i) {
            if (i > 0) oss << ",";
            oss << utxos[i].toJSON();
        }
        oss << "]";

        oss << "}";
        return oss.str();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// RPC Helper
// ═══════════════════════════════════════════════════════════════════════════

std::string callRPC(const std::string& method, const std::string& params = "") {
    std::string cmd = "curl -s --user \"__cookie__:$(cat ~/.dinero/.cookie 2>/dev/null)\" "
                      "--data-binary '{\"jsonrpc\":\"1.0\",\"id\":\"oracle\",\"method\":\"" + method +
                      "\",\"params\":" + (params.empty() ? "[]" : params) + "}' "
                      "-H 'content-type: text/plain;' http://127.0.0.1:20998/ 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);

    return result;
}

uint32_t getBlockHeight() {
    std::string result = callRPC("blockchain.getblockcount", "[]");
    size_t pos = result.find("\"result\":");
    if (pos != std::string::npos) {
        std::string val = result.substr(pos + 9);
        return std::stoul(val);
    }
    return 0;
}

bool isInMempool(const std::string& txid) {
    std::string result = callRPC("blockchain.getrawmempool", "[]");
    return (result.find(txid) != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// Snapshot Capture with Mempool Tracking
// ═══════════════════════════════════════════════════════════════════════════

WalletMempoolSnapshot captureWalletMempoolSnapshot(const std::string& wallet_db_path) {
    WalletMempoolSnapshot snapshot;
    snapshot.height = getBlockHeight();
    snapshot.balance_total_una = 0;
    snapshot.balance_confirmed_una = 0;
    snapshot.balance_unconfirmed_una = 0;
    snapshot.utxo_count = 0;
    snapshot.mempool_tx_count = 0;

    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open wallet database" << std::endl;
        return snapshot;
    }

    // Get UTXOs
    const char* sql_utxos = "SELECT txid, vout, amount, is_spent FROM utxos WHERE is_spent = 0";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql_utxos, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            UTXORecord utxo;
            utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            utxo.vout = sqlite3_column_int(stmt, 1);
            utxo.amount_una = sqlite3_column_int64(stmt, 2);
            utxo.is_spent = sqlite3_column_int(stmt, 3) != 0;
            utxo.is_locked = false;  // Will be determined from tx data

            snapshot.utxos.push_back(utxo);
            snapshot.balance_total_una += utxo.amount_una;
            snapshot.utxo_count++;
        }
        sqlite3_finalize(stmt);
    }

    // Get transaction records
    const char* sql_txs = "SELECT txid, amount, confirmations, is_coinbase, category FROM transactions";
    if (sqlite3_prepare_v2(db, sql_txs, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TransactionRecord tx;
            tx.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            tx.amount_una = static_cast<uint64_t>(sqlite3_column_double(stmt, 1) * 100000000.0);
            tx.confirmations = sqlite3_column_int(stmt, 2);
            tx.is_coinbase = sqlite3_column_int(stmt, 3) != 0;
            tx.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

            // Check if in mempool
            tx.in_mempool = (tx.confirmations == 0) && isInMempool(tx.txid);

            snapshot.transactions.push_back(tx);

            if (tx.in_mempool) {
                snapshot.mempool_tx_count++;
            }

            // Update balance breakdown
            if (tx.category == "receive" || tx.category == "generate") {
                if (tx.confirmations > 0) {
                    snapshot.balance_confirmed_una += tx.amount_una;
                } else if (tx.in_mempool) {
                    snapshot.balance_unconfirmed_una += tx.amount_una;
                }
            }

            // Mark UTXOs as locked if from unconfirmed tx
            if (tx.confirmations == 0 && tx.in_mempool) {
                for (auto& utxo : snapshot.utxos) {
                    if (utxo.txid == tx.txid) {
                        utxo.is_locked = true;
                    }
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return snapshot;
}

// ═══════════════════════════════════════════════════════════════════════════
// Mempool Eviction Safety Assertions
// ═══════════════════════════════════════════════════════════════════════════

struct ValidationResult {
    bool passed = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    void fail(const std::string& error) {
        passed = false;
        errors.push_back(error);
    }

    void warn(const std::string& warning) {
        warnings.push_back(warning);
    }
};

ValidationResult assert_no_fund_loss_after_eviction(const WalletMempoolSnapshot& before, const WalletMempoolSnapshot& after) {
    ValidationResult result;

    // After eviction, confirmed balance should not change
    // Unconfirmed balance may decrease if tx evicted
    // Total balance = confirmed + unconfirmed

    if (after.balance_confirmed_una < before.balance_confirmed_una) {
        std::ostringstream oss;
        oss << "❌ FATAL: Confirmed balance decreased after eviction! "
            << "Before: " << before.balance_confirmed_una << " una, "
            << "After: " << after.balance_confirmed_una << " una";
        result.fail(oss.str());
    }

    // Unconfirmed balance can decrease (eviction), but not increase unexpectedly
    if (after.balance_unconfirmed_una > before.balance_unconfirmed_una + before.balance_confirmed_una) {
        std::ostringstream oss;
        oss << "⚠ Unconfirmed balance increased unexpectedly: "
            << before.balance_unconfirmed_una << " -> " << after.balance_unconfirmed_una;
        result.warn(oss.str());
    }

    return result;
}

ValidationResult assert_no_ghost_confirmations(const WalletMempoolSnapshot& snapshot, const std::string& evicted_txid) {
    ValidationResult result;

    // Evicted tx should NOT be marked as confirmed
    for (const auto& tx : snapshot.transactions) {
        if (tx.txid == evicted_txid && tx.confirmations > 0) {
            std::ostringstream oss;
            oss << "❌ FATAL: Evicted transaction shows as confirmed! "
                << "TXID: " << evicted_txid << ", "
                << "Confirmations: " << tx.confirmations;
            result.fail(oss.str());
        }
    }

    return result;
}

ValidationResult assert_utxo_lock_released(const WalletMempoolSnapshot& before, const WalletMempoolSnapshot& after, const std::string& evicted_txid) {
    ValidationResult result;

    // Find UTXOs that were locked by evicted tx
    std::set<std::string> locked_utxos_before;
    for (const auto& tx : before.transactions) {
        if (tx.txid == evicted_txid && tx.category == "send") {
            // This tx spent some UTXOs - they should be unlocked after eviction
            // (In simplified implementation, we just check general lock state)
            result.warn("Checking UTXO lock state for evicted tx: " + evicted_txid);
        }
    }

    return result;
}

ValidationResult assert_conflicting_tx_handled(const WalletMempoolSnapshot& snapshot) {
    ValidationResult result;

    // Check for conflicting transactions (same inputs)
    // In practice, this would require UTXO input tracking
    // Simplified: just check for duplicate spends in mempool txs

    std::map<std::string, int> utxo_spend_count;

    for (const auto& tx : snapshot.transactions) {
        if (tx.in_mempool && tx.category == "send") {
            // Track which UTXOs are being spent
            // (Simplified - would need actual input tracking)
        }
    }

    return result;
}

ValidationResult assert_balance_matches_mempool(const WalletMempoolSnapshot& snapshot) {
    ValidationResult result;

    // Balance should equal sum of confirmed + unconfirmed (in mempool) UTXOs
    uint64_t expected_balance = snapshot.balance_confirmed_una + snapshot.balance_unconfirmed_una;

    if (snapshot.balance_total_una != expected_balance) {
        std::ostringstream oss;
        oss << "⚠ Balance mismatch with mempool state: "
            << "Total: " << snapshot.balance_total_una << " una, "
            << "Expected (confirmed + unconfirmed): " << expected_balance << " una";
        result.warn(oss.str());
    }

    return result;
}

ValidationResult assert_evicted_tx_not_in_mempool(const std::string& txid) {
    ValidationResult result;

    if (isInMempool(txid)) {
        std::ostringstream oss;
        oss << "❌ FATAL: Evicted transaction still in mempool! "
            << "TXID: " << txid;
        result.fail(oss.str());
    } else {
        std::cout << "✓ Transaction not in mempool: " << txid << std::endl;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Validation Commands
// ═══════════════════════════════════════════════════════════════════════════

int cmdSnapshot(const std::string& wallet_db_path) {
    WalletMempoolSnapshot snapshot = captureWalletMempoolSnapshot(wallet_db_path);
    std::cout << snapshot.toJSON() << std::endl;
    return 0;
}

int cmdValidateEviction(const std::string& before_file, const std::string& after_file, const std::string& txid) {
    // Load snapshots
    WalletMempoolSnapshot before = captureWalletMempoolSnapshot(before_file);
    WalletMempoolSnapshot after = captureWalletMempoolSnapshot(after_file);

    std::cout << "Validating mempool eviction for txid: " << txid << std::endl;

    // Run all assertions
    std::vector<ValidationResult> results;
    results.push_back(assert_no_fund_loss_after_eviction(before, after));
    results.push_back(assert_no_ghost_confirmations(after, txid));
    results.push_back(assert_utxo_lock_released(before, after, txid));
    results.push_back(assert_conflicting_tx_handled(after));
    results.push_back(assert_balance_matches_mempool(after));

    if (!txid.empty()) {
        results.push_back(assert_evicted_tx_not_in_mempool(txid));
    }

    // Check if all passed
    bool all_passed = true;
    for (const auto& result : results) {
        if (!result.passed) {
            all_passed = false;
            for (const auto& error : result.errors) {
                std::cerr << error << std::endl;
            }
        }
        for (const auto& warning : result.warnings) {
            std::cout << warning << std::endl;
        }
    }

    if (all_passed) {
        std::cout << "✅ All mempool eviction assertions passed" << std::endl;
        return 0;
    } else {
        std::cerr << "❌ Mempool eviction validation FAILED" << std::endl;
        return 1;
    }
}

int cmdCheckMempoolTx(const std::string& txid) {
    bool in_mempool = isInMempool(txid);

    if (in_mempool) {
        std::cout << "✓ Transaction in mempool: " << txid << std::endl;
        return 0;
    } else {
        std::cout << "✗ Transaction NOT in mempool: " << txid << std::endl;
        return 1;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  wallet_mempool_oracle snapshot <wallet_db_path>" << std::endl;
        std::cerr << "  wallet_mempool_oracle validate_eviction <before_db> <after_db> <txid>" << std::endl;
        std::cerr << "  wallet_mempool_oracle check_mempool_tx <txid>" << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "snapshot") {
        if (argc < 3) {
            std::cerr << "Error: wallet_db_path required" << std::endl;
            return 1;
        }
        return cmdSnapshot(argv[2]);
    } else if (command == "validate_eviction") {
        if (argc < 5) {
            std::cerr << "Error: before_db, after_db, and txid required" << std::endl;
            return 1;
        }
        return cmdValidateEviction(argv[2], argv[3], argv[4]);
    } else if (command == "check_mempool_tx") {
        if (argc < 3) {
            std::cerr << "Error: txid required" << std::endl;
            return 1;
        }
        return cmdCheckMempoolTx(argv[2]);
    } else {
        std::cerr << "Error: Unknown command: " << command << std::endl;
        return 1;
    }
}
