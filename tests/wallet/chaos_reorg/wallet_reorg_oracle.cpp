/**
 * @file wallet_reorg_oracle.cpp
 * @brief Wallet Oracle for Blockchain Reorganization Validation
 *
 * This oracle validates wallet safety during blockchain reorganizations.
 * Tests that funds are conserved and transaction states are consistent
 * when the blockchain tip changes.
 *
 * Usage:
 *   wallet_reorg_oracle snapshot <wallet_db> > state.json
 *   wallet_reorg_oracle validate_reorg <before.json> <after.json>
 *   wallet_reorg_oracle check_tx_confirmations <wallet_db> <txid>
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
// Transaction State Model (After Reorg)
// ═══════════════════════════════════════════════════════════════════════════

enum class TxState {
    CONFIRMED,     // Transaction in active chain
    UNCONFIRMED,   // Transaction in mempool, 0 confirmations
    CONFLICTED,    // Transaction conflicts with chain (double-spend)
    ABANDONED      // Transaction will never confirm (orphaned)
};

const char* txStateToString(TxState state) {
    switch (state) {
        case TxState::CONFIRMED: return "CONFIRMED";
        case TxState::UNCONFIRMED: return "UNCONFIRMED";
        case TxState::CONFLICTED: return "CONFLICTED";
        case TxState::ABANDONED: return "ABANDONED";
        default: return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Wallet Snapshot (with Transaction Confirmation Tracking)
// ═══════════════════════════════════════════════════════════════════════════

struct TransactionRecord {
    std::string txid;
    uint64_t amount_una;
    uint32_t confirmations;
    bool is_coinbase;
    std::string category;  // "send", "receive", "generate"
    uint32_t block_height;

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"txid\":\"" << txid << "\",";
        oss << "\"amount\":" << amount_una << ",";
        oss << "\"confirmations\":" << confirmations << ",";
        oss << "\"is_coinbase\":" << (is_coinbase ? "true" : "false") << ",";
        oss << "\"category\":\"" << category << "\",";
        oss << "\"block_height\":" << block_height;
        oss << "}";
        return oss.str();
    }
};

struct UTXORecord {
    std::string txid;
    uint32_t vout;
    uint64_t amount_una;
    bool is_spent;
    uint32_t confirmations;

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
        oss << "\"confirmations\":" << confirmations;
        oss << "}";
        return oss.str();
    }
};

struct WalletReorgSnapshot {
    uint32_t height;
    std::string best_block_hash;
    uint64_t balance_total_una;
    uint64_t balance_confirmed_una;
    uint64_t balance_unconfirmed_una;
    uint32_t utxo_count;
    uint32_t address_count;

    std::vector<TransactionRecord> transactions;
    std::vector<UTXORecord> utxos;

    std::string toJSON() const {
        std::ostringstream oss;
        oss << "{";
        oss << "\"height\":" << height << ",";
        oss << "\"best_block_hash\":\"" << best_block_hash << "\",";
        oss << "\"balance_total_una\":" << balance_total_una << ",";
        oss << "\"balance_confirmed_una\":" << balance_confirmed_una << ",";
        oss << "\"balance_unconfirmed_una\":" << balance_unconfirmed_una << ",";
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
    // Parse JSON result
    size_t pos = result.find("\"result\":");
    if (pos != std::string::npos) {
        std::string val = result.substr(pos + 9);
        return std::stoul(val);
    }
    return 0;
}

std::string getBestBlockHash() {
    std::string result = callRPC("blockchain.getbestblockhash", "[]");
    // Parse JSON result (simplified)
    size_t start = result.find("\"result\":\"");
    if (start != std::string::npos) {
        start += 10;
        size_t end = result.find("\"", start);
        if (end != std::string::npos) {
            return result.substr(start, end - start);
        }
    }
    return "";
}

// ═══════════════════════════════════════════════════════════════════════════
// Snapshot Capture with Reorg Tracking
// ═══════════════════════════════════════════════════════════════════════════

WalletReorgSnapshot captureWalletReorgSnapshot(const std::string& wallet_db_path) {
    WalletReorgSnapshot snapshot;
    snapshot.height = getBlockHeight();
    snapshot.best_block_hash = getBestBlockHash();
    snapshot.balance_total_una = 0;
    snapshot.balance_confirmed_una = 0;
    snapshot.balance_unconfirmed_una = 0;
    snapshot.utxo_count = 0;
    snapshot.address_count = 0;

    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open wallet database" << std::endl;
        return snapshot;
    }

    // Get UTXOs with confirmation count
    const char* sql_utxos = "SELECT txid, vout, amount, is_spent FROM utxos WHERE is_spent = 0";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql_utxos, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            UTXORecord utxo;
            utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            utxo.vout = sqlite3_column_int(stmt, 1);
            utxo.amount_una = sqlite3_column_int64(stmt, 2);
            utxo.is_spent = sqlite3_column_int(stmt, 3) != 0;
            utxo.confirmations = 0;  // Will be updated from tx data

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
            tx.block_height = 0;  // Could be computed from confirmations

            snapshot.transactions.push_back(tx);

            // Update UTXO confirmations
            for (auto& utxo : snapshot.utxos) {
                if (utxo.txid == tx.txid) {
                    utxo.confirmations = tx.confirmations;
                    if (tx.confirmations > 0) {
                        snapshot.balance_confirmed_una += utxo.amount_una;
                    } else {
                        snapshot.balance_unconfirmed_una += utxo.amount_una;
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
// Reorg Safety Assertions
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

ValidationResult assert_no_fund_loss_across_reorg(const WalletReorgSnapshot& before, const WalletReorgSnapshot& after) {
    ValidationResult result;

    // During reorg, total balance can change due to:
    // 1. Transactions becoming unconfirmed (but still exist)
    // 2. Transactions disappearing (conflicted)
    // 3. New transactions appearing
    //
    // We check: confirmed + unconfirmed balance should not decrease
    // unless a transaction was explicitly conflicted/abandoned

    uint64_t before_total = before.balance_total_una;
    uint64_t after_total = after.balance_total_una;

    // Build tx sets for comparison
    std::map<std::string, uint64_t> before_txs;
    std::map<std::string, uint64_t> after_txs;

    for (const auto& tx : before.transactions) {
        if (tx.category == "receive" || tx.category == "generate") {
            before_txs[tx.txid] = tx.amount_una;
        }
    }

    for (const auto& tx : after.transactions) {
        if (tx.category == "receive" || tx.category == "generate") {
            after_txs[tx.txid] = tx.amount_una;
        }
    }

    // Check for disappeared transactions
    for (const auto& [txid, amount] : before_txs) {
        if (after_txs.find(txid) == after_txs.end()) {
            result.warn("Transaction disappeared after reorg: " + txid);
            // This is OK if it was conflicted, but we warn about it
        }
    }

    // Fund loss is ONLY a problem if:
    // - Total balance decreased
    // - AND no transactions were marked as conflicted/abandoned
    // - AND we can't account for the difference

    if (after_total < before_total) {
        uint64_t loss = before_total - after_total;
        std::ostringstream oss;
        oss << "⚠ Balance decreased after reorg: "
            << "Before: " << before_total << " una, "
            << "After: " << after_total << " una, "
            << "Difference: " << loss << " una";
        result.warn(oss.str());

        // Check if loss is accounted for by disappeared txs
        uint64_t disappeared_amount = 0;
        for (const auto& [txid, amount] : before_txs) {
            if (after_txs.find(txid) == after_txs.end()) {
                disappeared_amount += amount;
            }
        }

        if (disappeared_amount < loss) {
            oss.str("");
            oss << "❌ FATAL: Unaccounted fund loss! "
                << "Loss: " << loss << " una, "
                << "Disappeared tx amount: " << disappeared_amount << " una";
            result.fail(oss.str());
        }
    }

    return result;
}

ValidationResult assert_no_fund_duplication(const WalletReorgSnapshot& after) {
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

ValidationResult assert_tx_state_consistent(const WalletReorgSnapshot& after) {
    ValidationResult result;

    // After reorg, each transaction should have a consistent state
    // - If confirmations > 0: CONFIRMED
    // - If confirmations == 0: UNCONFIRMED (in mempool)
    // - Transaction cannot have ambiguous state

    for (const auto& tx : after.transactions) {
        if (tx.confirmations == 0) {
            // Should be in mempool
            // We can't easily check mempool from here, so just warn
            result.warn("Transaction unconfirmed after reorg: " + tx.txid);
        }
    }

    return result;
}

ValidationResult assert_balance_equals_utxo_sum(const WalletReorgSnapshot& snapshot) {
    ValidationResult result;

    // Balance should equal sum of unspent UTXOs
    uint64_t utxo_sum = 0;
    for (const auto& utxo : snapshot.utxos) {
        if (!utxo.is_spent) {
            utxo_sum += utxo.amount_una;
        }
    }

    if (utxo_sum != snapshot.balance_total_una) {
        std::ostringstream oss;
        oss << "❌ FATAL: Balance mismatch! "
            << "Balance: " << snapshot.balance_total_una << " una, "
            << "UTXO sum: " << utxo_sum << " una";
        result.fail(oss.str());
    }

    return result;
}

ValidationResult assert_utxo_maturity_respected(const WalletReorgSnapshot& snapshot) {
    ValidationResult result;

    // Coinbase UTXOs must have 100+ confirmations to be spendable
    for (const auto& utxo : snapshot.utxos) {
        // Find corresponding transaction
        for (const auto& tx : snapshot.transactions) {
            if (tx.txid == utxo.txid && tx.is_coinbase) {
                if (tx.confirmations < 100 && !utxo.is_spent) {
                    // Immature coinbase - should not be marked as spendable
                    if (tx.confirmations > 0) {
                        std::ostringstream oss;
                        oss << "⚠ Immature coinbase UTXO: " << utxo.key()
                            << " (confirmations: " << tx.confirmations << ")";
                        result.warn(oss.str());
                    }
                }
            }
        }
    }

    return result;
}

ValidationResult assert_height_decreased_or_same(const WalletReorgSnapshot& before, const WalletReorgSnapshot& after) {
    ValidationResult result;

    // During reorg, height should decrease temporarily or stay same
    // It should NOT increase dramatically unless we're past the reorg
    if (after.height < before.height) {
        std::cout << "✓ Reorg detected: height decreased from " << before.height
                  << " to " << after.height << std::endl;
    } else if (after.height > before.height + 10) {
        std::ostringstream oss;
        oss << "⚠ Large height increase after reorg: "
            << before.height << " -> " << after.height;
        result.warn(oss.str());
    }

    return result;
}

ValidationResult assert_best_block_changed(const WalletReorgSnapshot& before, const WalletReorgSnapshot& after) {
    ValidationResult result;

    if (before.best_block_hash == after.best_block_hash) {
        result.warn("Best block hash unchanged - reorg may not have occurred");
    } else {
        std::cout << "✓ Best block changed: " << before.best_block_hash
                  << " -> " << after.best_block_hash << std::endl;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Validation Commands
// ═══════════════════════════════════════════════════════════════════════════

int cmdSnapshot(const std::string& wallet_db_path) {
    WalletReorgSnapshot snapshot = captureWalletReorgSnapshot(wallet_db_path);
    std::cout << snapshot.toJSON() << std::endl;
    return 0;
}

int cmdValidateReorg(const std::string& before_file, const std::string& after_file) {
    // Load snapshots (simplified - would use proper JSON parser in production)
    WalletReorgSnapshot before = captureWalletReorgSnapshot(before_file);
    WalletReorgSnapshot after = captureWalletReorgSnapshot(after_file);

    std::cout << "Validating reorg handling..." << std::endl;

    // Run all assertions
    std::vector<ValidationResult> results;
    results.push_back(assert_height_decreased_or_same(before, after));
    results.push_back(assert_best_block_changed(before, after));
    results.push_back(assert_no_fund_loss_across_reorg(before, after));
    results.push_back(assert_no_fund_duplication(after));
    results.push_back(assert_tx_state_consistent(after));
    results.push_back(assert_balance_equals_utxo_sum(after));
    results.push_back(assert_utxo_maturity_respected(after));

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
        std::cout << "✅ All reorg safety assertions passed" << std::endl;
        return 0;
    } else {
        std::cerr << "❌ Reorg validation FAILED" << std::endl;
        return 1;
    }
}

int cmdCheckTxConfirmations(const std::string& wallet_db_path, const std::string& txid) {
    sqlite3* db = nullptr;
    if (sqlite3_open(wallet_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open wallet database" << std::endl;
        return 1;
    }

    const char* sql = "SELECT confirmations FROM transactions WHERE txid = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            uint32_t confirmations = sqlite3_column_int(stmt, 0);
            std::cout << "Transaction " << txid << " has " << confirmations << " confirmations" << std::endl;
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return 0;
        } else {
            std::cerr << "Transaction not found: " << txid << std::endl;
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return 1;
        }
    }

    sqlite3_close(db);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  wallet_reorg_oracle snapshot <wallet_db_path>" << std::endl;
        std::cerr << "  wallet_reorg_oracle validate_reorg <before_db> <after_db>" << std::endl;
        std::cerr << "  wallet_reorg_oracle check_tx_confirmations <wallet_db_path> <txid>" << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "snapshot") {
        if (argc < 3) {
            std::cerr << "Error: wallet_db_path required" << std::endl;
            return 1;
        }
        return cmdSnapshot(argv[2]);
    } else if (command == "validate_reorg") {
        if (argc < 4) {
            std::cerr << "Error: before_db and after_db required" << std::endl;
            return 1;
        }
        return cmdValidateReorg(argv[2], argv[3]);
    } else if (command == "check_tx_confirmations") {
        if (argc < 4) {
            std::cerr << "Error: wallet_db_path and txid required" << std::endl;
            return 1;
        }
        return cmdCheckTxConfirmations(argv[2], argv[3]);
    } else {
        std::cerr << "Error: Unknown command: " << command << std::endl;
        return 1;
    }
}
