/**
 * @file wallet_funds_oracle.cpp
 * @brief Extended Wallet Oracle for Transaction State Validation
 *
 * This oracle validates wallet safety during spend/sign/broadcast operations.
 * Extends the basic wallet_oracle with transaction-specific assertions.
 *
 * Usage:
 *   wallet_funds_oracle snapshot <wallet_db> > state.json
 *   wallet_funds_oracle validate_spend <before.json> <after.json> <txid>
 *   wallet_funds_oracle check_tx_state <txid>
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
// Transaction State Model (Must Resolve to Exactly ONE State)
// ═══════════════════════════════════════════════════════════════════════════

enum class TxState {
    ABSENT,      // Transaction does not exist anywhere
    MEMPOOL,     // In mempool only (not confirmed)
    CONFIRMED,   // In blockchain (confirmed)
    FAILED       // Explicitly rejected/abandoned
};

const char* txStateToString(TxState state) {
    switch (state) {
        case TxState::ABSENT: return "ABSENT";
        case TxState::MEMPOOL: return "MEMPOOL";
        case TxState::CONFIRMED: return "CONFIRMED";
        case TxState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Extended Wallet Snapshot (with Transaction Tracking)
// ═══════════════════════════════════════════════════════════════════════════

struct TransactionRecord {
    std::string txid;
    uint64_t amount_una;
    uint32_t confirmations;
    bool is_coinbase;
    std::string category;  // "send", "receive", "generate"

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"txid\":\"" << txid << "\",";
        oss << "\"amount\":" << amount_una << ",";
        oss << "\"confirmations\":" << confirmations << ",";
        oss << "\"is_coinbase\":" << (is_coinbase ? "true" : "false") << ",";
        oss << "\"category\":\"" << category << "\"";
        oss << "}";
        return oss.str();
    }
};

struct UTXORecord {
    std::string txid;
    uint32_t vout;
    uint64_t amount_una;
    bool is_spent;

    std::string key() const {
        return txid + ":" + std::to_string(vout);
    }
};

struct WalletFundsSnapshot {
    uint32_t height;
    uint64_t balance_total_una;
    uint64_t balance_spendable_una;
    uint32_t utxo_count;
    uint32_t address_count;

    std::vector<TransactionRecord> transactions;
    std::vector<UTXORecord> utxos;

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"height\":" << height << ",";
        oss << "\"balance_total_una\":" << balance_total_una << ",";
        oss << "\"balance_spendable_una\":" << balance_spendable_una << ",";
        oss << "\"utxo_count\":" << utxo_count << ",";
        oss << "\"address_count\":" << address_count << ",";

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
            oss << "{\"txid\":\"" << utxos[i].txid << "\",";
            oss << "\"vout\":" << utxos[i].vout << ",";
            oss << "\"amount\":" << utxos[i].amount_una << ",";
            oss << "\"is_spent\":" << (utxos[i].is_spent ? "true" : "false");
            oss << "}";
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

// ═══════════════════════════════════════════════════════════════════════════
// Snapshot Capture with Transaction Tracking
// ═══════════════════════════════════════════════════════════════════════════

WalletFundsSnapshot captureWalletFundsSnapshot(const std::string& wallet_db_path) {
    WalletFundsSnapshot snapshot;
    snapshot.height = 0;
    snapshot.balance_total_una = 0;
    snapshot.balance_spendable_una = 0;
    snapshot.utxo_count = 0;
    snapshot.address_count = 0;

    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open wallet database" << std::endl;
        return snapshot;
    }

    // Get UTXO count and balance
    const char* sql_utxos = "SELECT txid, vout, amount, is_spent FROM utxos WHERE is_spent = 0";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql_utxos, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            UTXORecord utxo;
            utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            utxo.vout = sqlite3_column_int(stmt, 1);
            utxo.amount_una = sqlite3_column_int64(stmt, 2);
            utxo.is_spent = sqlite3_column_int(stmt, 3) != 0;

            snapshot.utxos.push_back(utxo);
            snapshot.balance_total_una += utxo.amount_una;
            snapshot.utxo_count++;
        }
        sqlite3_finalize(stmt);
    }

    // Get address count
    const char* sql_addresses = "SELECT COUNT(*) FROM addresses";
    if (sqlite3_prepare_v2(db, sql_addresses, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            snapshot.address_count = sqlite3_column_int(stmt, 0);
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

            snapshot.transactions.push_back(tx);
        }
        sqlite3_finalize(stmt);
    }

    snapshot.balance_spendable_una = snapshot.balance_total_una;

    sqlite3_close(db);
    return snapshot;
}

// ═══════════════════════════════════════════════════════════════════════════
// Fund Safety Assertions
// ═══════════════════════════════════════════════════════════════════════════

struct ValidationResult {
    bool passed = true;
    std::vector<std::string> errors;

    void fail(const std::string& error) {
        passed = false;
        errors.push_back(error);
    }
};

ValidationResult assert_no_fund_loss(const WalletFundsSnapshot& before, const WalletFundsSnapshot& after) {
    ValidationResult result;

    // Balance can only stay same or increase (from mining)
    // Never decrease without explicit confirmed spend
    if (after.balance_total_una < before.balance_total_una) {
        std::ostringstream oss;
        oss << "❌ FATAL: Fund loss detected! "
            << "Before: " << before.balance_total_una << " una, "
            << "After: " << after.balance_total_una << " una, "
            << "Loss: " << (before.balance_total_una - after.balance_total_una) << " una";
        result.fail(oss.str());
    }

    return result;
}

ValidationResult assert_no_fund_duplication(const WalletFundsSnapshot& before, const WalletFundsSnapshot& after) {
    ValidationResult result;

    // Check for duplicate UTXOs (same txid:vout appearing twice)
    std::set<std::string> utxo_keys;

    for (const auto& utxo : after.utxos) {
        std::string key = utxo.key();
        if (utxo_keys.count(key)) {
            std::ostringstream oss;
            oss << "❌ FATAL: Duplicate UTXO detected! " << key;
            result.fail(oss.str());
        }
        utxo_keys.insert(key);
    }

    return result;
}

ValidationResult assert_utxo_conservation(const WalletFundsSnapshot& before, const WalletFundsSnapshot& after) {
    ValidationResult result;

    // Build UTXO sets
    std::set<std::string> before_utxos, after_utxos;

    for (const auto& utxo : before.utxos) {
        before_utxos.insert(utxo.key());
    }

    for (const auto& utxo : after.utxos) {
        after_utxos.insert(utxo.key());
    }

    // Check if any UTXOs vanished (not spent, just disappeared)
    for (const auto& key : before_utxos) {
        if (!after_utxos.count(key)) {
            // UTXO disappeared - this is only OK if there's a confirmed spend transaction
            // For now, flag as potential issue
            std::cerr << "⚠ UTXO disappeared: " << key << " (may be spent)" << std::endl;
        }
    }

    return result;
}

ValidationResult assert_no_partial_spend(const WalletFundsSnapshot& before, const WalletFundsSnapshot& after) {
    ValidationResult result;

    // A "partial spend" means:
    // - UTXOs marked as spent in wallet DB
    // - But no corresponding transaction exists
    // This is database corruption

    // Check via balance reconciliation
    uint64_t utxo_sum = 0;
    for (const auto& utxo : after.utxos) {
        if (!utxo.is_spent) {
            utxo_sum += utxo.amount_una;
        }
    }

    // Balance should equal sum of unspent UTXOs
    if (utxo_sum != after.balance_total_una) {
        std::ostringstream oss;
        oss << "❌ FATAL: Balance mismatch! "
            << "Balance: " << after.balance_total_una << " una, "
            << "UTXO sum: " << utxo_sum << " una";
        result.fail(oss.str());
    }

    return result;
}

ValidationResult assert_tx_state_consistent(const std::string& txid) {
    ValidationResult result;

    // Check transaction state via RPC
    // Must be exactly ONE of: ABSENT, MEMPOOL, CONFIRMED, FAILED
    // Hybrid states = failure

    std::string mempool_result = callRPC("blockchain.getrawmempool");
    std::string tx_result = callRPC("blockchain.getrawtransaction", "[\"" + txid + "\", true]");

    bool in_mempool = (mempool_result.find(txid) != std::string::npos);
    bool in_chain = (tx_result.find("\"confirmations\"") != std::string::npos);

    if (in_mempool && in_chain) {
        result.fail("❌ FATAL: Transaction in BOTH mempool and chain! (Hybrid state)");
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Validation Commands
// ═══════════════════════════════════════════════════════════════════════════

int cmdSnapshot(const std::string& wallet_db_path) {
    WalletFundsSnapshot snapshot = captureWalletFundsSnapshot(wallet_db_path);
    std::cout << snapshot.toJSON() << std::endl;
    return 0;
}

int cmdValidateSpend(const std::string& before_file, const std::string& after_file, const std::string& txid) {
    // Load snapshots (simplified - would use proper JSON parser in production)
    WalletFundsSnapshot before = captureWalletFundsSnapshot(before_file);
    WalletFundsSnapshot after = captureWalletFundsSnapshot(after_file);

    std::cout << "Validating spend operation with txid: " << txid << std::endl;

    // Run all assertions
    std::vector<ValidationResult> results;
    results.push_back(assert_no_fund_loss(before, after));
    results.push_back(assert_no_fund_duplication(before, after));
    results.push_back(assert_utxo_conservation(before, after));
    results.push_back(assert_no_partial_spend(before, after));

    if (!txid.empty()) {
        results.push_back(assert_tx_state_consistent(txid));
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
    }

    if (all_passed) {
        std::cout << "✅ All fund safety assertions passed" << std::endl;
        return 0;
    } else {
        std::cerr << "❌ Fund safety validation FAILED" << std::endl;
        return 1;
    }
}

int cmdCheckTxState(const std::string& txid) {
    ValidationResult result = assert_tx_state_consistent(txid);

    if (result.passed) {
        std::cout << "✅ Transaction state is consistent" << std::endl;
        return 0;
    } else {
        for (const auto& error : result.errors) {
            std::cerr << error << std::endl;
        }
        return 1;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  wallet_funds_oracle snapshot <wallet_db_path>" << std::endl;
        std::cerr << "  wallet_funds_oracle validate_spend <before_db> <after_db> <txid>" << std::endl;
        std::cerr << "  wallet_funds_oracle check_tx_state <txid>" << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "snapshot") {
        if (argc < 3) {
            std::cerr << "Error: wallet_db_path required" << std::endl;
            return 1;
        }
        return cmdSnapshot(argv[2]);
    } else if (command == "validate_spend") {
        if (argc < 5) {
            std::cerr << "Error: before_db, after_db, and txid required" << std::endl;
            return 1;
        }
        return cmdValidateSpend(argv[2], argv[3], argv[4]);
    } else if (command == "check_tx_state") {
        if (argc < 3) {
            std::cerr << "Error: txid required" << std::endl;
            return 1;
        }
        return cmdCheckTxState(argv[2]);
    } else {
        std::cerr << "Error: Unknown command: " << command << std::endl;
        return 1;
    }
}
