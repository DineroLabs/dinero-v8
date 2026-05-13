#include "wallet/wallet_balances.h"
#include "common/logger.h"
#include <stdexcept>

namespace dinero {

static long long scalarQuery(sqlite3* db, const char* sql, int bindHeight) {
    sqlite3_stmt* st = nullptr;
    long long v = 0;
    
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (bindHeight >= 0) {
            sqlite3_bind_int(st, 1, bindHeight);
        }
        if (sqlite3_step(st) == SQLITE_ROW) {
            v = sqlite3_column_int64(st, 0);
        }
    }
    
    if (st) {
        sqlite3_finalize(st);
    }
    
    return v;
}

bool compute_wallet_balances(sqlite3* db, int tipHeight, WalletBalances& out) {
    if (!db) {
        g_logger.error("Cannot compute balances: null database");
        return false;
    }
    
    try {
        // Confirmed (on-chain, AND coinbase matured if spendable_at exists)
        out.confirmed = scalarQuery(db,
            "SELECT COALESCE(SUM(o.value),0) "
            "FROM wallet_txouts o JOIN wallet_txs t ON t.txid=o.txid "
            "WHERE t.height IS NOT NULL AND (o.spendable_at IS NULL OR o.spendable_at <= ?);",
            tipHeight);

        // Immature coinbase (not yet spendable)
        out.immature = scalarQuery(db,
            "SELECT COALESCE(SUM(o.value),0) "
            "FROM wallet_txouts o JOIN wallet_txs t ON t.txid=o.txid "
            "WHERE t.height IS NOT NULL AND o.spendable_at IS NOT NULL AND o.spendable_at > ?;",
            tipHeight);

        // Unconfirmed (mempool)
        out.unconfirmed = scalarQuery(db,
            "SELECT COALESCE(SUM(o.value),0) "
            "FROM wallet_txouts o JOIN wallet_txs t ON t.txid=o.txid "
            "WHERE t.height IS NULL;",
            -1);

        g_logger.debug("Computed wallet balances - Confirmed: " + std::to_string(out.confirmed) +
                      ", Immature: " + std::to_string(out.immature) +
                      ", Unconfirmed: " + std::to_string(out.unconfirmed));
        
        return true;
    } catch (const std::exception& e) {
        g_logger.error("Error computing wallet balances: " + std::string(e.what()));
        return false;
    }
}

Json::Value balances_to_json(const WalletBalances& b) {
    Json::Value j(Json::objectValue);
    j["confirmed"]   = int64_t(b.confirmed);
    j["immature"]    = int64_t(b.immature);
    j["unconfirmed"] = int64_t(b.unconfirmed);
    j["total"]       = int64_t(b.total());
    return j;
}

bool migrate_wallet_balances(sqlite3* db) {
    if (!db) {
        g_logger.error("Cannot migrate wallet balances: null database");
        return false;
    }
    
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS wallet_txs (
          txid BLOB PRIMARY KEY,         -- raw 32 bytes (little-endian or canonical)
          height INTEGER,                -- NULL if mempool
          is_coinbase INTEGER NOT NULL DEFAULT 0,
          total_received INTEGER NOT NULL DEFAULT 0
        );
        
        CREATE TABLE IF NOT EXISTS wallet_txouts (
          txid BLOB NOT NULL,
          n INTEGER NOT NULL,
          value INTEGER NOT NULL,
          script TEXT NOT NULL,
          spendable_at INTEGER,          -- for coinbase: height + COINBASE_MATURITY
          PRIMARY KEY(txid, n)
        );
        
        CREATE INDEX IF NOT EXISTS idx_wallet_txs_height ON wallet_txs(height);
        CREATE INDEX IF NOT EXISTS idx_wallet_txouts_spendable ON wallet_txouts(spendable_at);
    )SQL";
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    
    if (rc != SQLITE_OK) {
        std::string error = "Failed to migrate wallet balance tables: ";
        if (errMsg) {
            error += errMsg;
            sqlite3_free(errMsg);
        }
        g_logger.error(error);
        return false;
    }
    
    g_logger.info("Wallet balance tables migrated successfully");
    return true;
}

bool add_wallet_transaction(sqlite3* db, 
                           const std::vector<uint8_t>& txid,
                           std::optional<int> height,
                           bool is_coinbase,
                           const std::vector<std::pair<uint32_t, int64_t>>& outputs,
                           int coinbase_maturity_height) {
    if (!db || txid.size() != 32) {
        g_logger.error("Invalid parameters for add_wallet_transaction");
        return false;
    }
    
    // Begin atomic transaction
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.error("Failed to begin wallet transaction: " + std::string(err_msg ? err_msg : "unknown"));
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    
    bool success = false;
    
    try {
        // Calculate total received
        int64_t total_received = 0;
        for (const auto& output : outputs) {
            total_received += output.second;
        }
        
        // Insert/update wallet_txs
        const char* tx_sql = R"SQL(
            INSERT INTO wallet_txs(txid, height, is_coinbase, total_received)
            VALUES(?, ?, ?, ?)
            ON CONFLICT(txid) DO UPDATE SET 
                height = excluded.height,
                is_coinbase = excluded.is_coinbase,
                total_received = excluded.total_received;
        )SQL";
        
        sqlite3_stmt* tx_stmt = nullptr;
        rc = sqlite3_prepare_v2(db, tx_sql, -1, &tx_stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare tx statement");
        }
        
        sqlite3_bind_blob(tx_stmt, 1, txid.data(), static_cast<int>(txid.size()), SQLITE_STATIC);
        if (height) {
            sqlite3_bind_int(tx_stmt, 2, *height);
        } else {
            sqlite3_bind_null(tx_stmt, 2);
        }
        sqlite3_bind_int(tx_stmt, 3, is_coinbase ? 1 : 0);
        sqlite3_bind_int64(tx_stmt, 4, total_received);
        
        rc = sqlite3_step(tx_stmt);
        sqlite3_finalize(tx_stmt);
        
        if (rc != SQLITE_DONE) {
            throw std::runtime_error("Failed to insert wallet transaction");
        }
        
        // Insert outputs
        const char* out_sql = R"SQL(
            INSERT OR REPLACE INTO wallet_txouts(txid, n, value, script, spendable_at)
            VALUES(?, ?, ?, ?, ?);
        )SQL";
        
        sqlite3_stmt* out_stmt = nullptr;
        rc = sqlite3_prepare_v2(db, out_sql, -1, &out_stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare output statement");
        }
        
        for (const auto& output : outputs) {
            uint32_t n = output.first;
            int64_t value = output.second;
            
            sqlite3_bind_blob(out_stmt, 1, txid.data(), static_cast<int>(txid.size()), SQLITE_STATIC);
            sqlite3_bind_int(out_stmt, 2, static_cast<int>(n));
            sqlite3_bind_int64(out_stmt, 3, value);
            sqlite3_bind_text(out_stmt, 4, "", -1, SQLITE_STATIC); // Script placeholder
            
            if (is_coinbase && coinbase_maturity_height > 0) {
                sqlite3_bind_int(out_stmt, 5, coinbase_maturity_height);
            } else {
                sqlite3_bind_null(out_stmt, 5);
            }
            
            rc = sqlite3_step(out_stmt);
            if (rc != SQLITE_DONE) {
                sqlite3_finalize(out_stmt);
                throw std::runtime_error("Failed to insert wallet output");
            }
            
            sqlite3_reset(out_stmt);
        }
        
        sqlite3_finalize(out_stmt);
        success = true;
        
    } catch (const std::exception& e) {
        g_logger.error("Error in add_wallet_transaction: " + std::string(e.what()));
    }
    
    // Commit or rollback
    if (success) {
        rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            g_logger.error("Failed to commit wallet transaction: " + std::string(err_msg ? err_msg : "unknown"));
            if (err_msg) sqlite3_free(err_msg);
            success = false;
        }
    } else {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    
    return success;
}

bool remove_wallet_transactions_from_height(sqlite3* db, int min_height) {
    if (!db) {
        g_logger.error("Invalid database for remove_wallet_transactions_from_height");
        return false;
    }
    
    // Begin atomic transaction
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.error("Failed to begin removal transaction: " + std::string(err_msg ? err_msg : "unknown"));
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }
    
    bool success = false;
    
    try {
        // Remove outputs first (due to foreign key constraints)
        const char* remove_outputs_sql = R"SQL(
            DELETE FROM wallet_txouts 
            WHERE txid IN (
                SELECT txid FROM wallet_txs WHERE height >= ?
            );
        )SQL";
        
        sqlite3_stmt* out_stmt = nullptr;
        rc = sqlite3_prepare_v2(db, remove_outputs_sql, -1, &out_stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare output removal statement");
        }
        
        sqlite3_bind_int(out_stmt, 1, min_height);
        rc = sqlite3_step(out_stmt);
        sqlite3_finalize(out_stmt);
        
        if (rc != SQLITE_DONE) {
            throw std::runtime_error("Failed to remove wallet outputs");
        }
        
        // Remove transactions
        const char* remove_txs_sql = "DELETE FROM wallet_txs WHERE height >= ?;";
        
        sqlite3_stmt* tx_stmt = nullptr;
        rc = sqlite3_prepare_v2(db, remove_txs_sql, -1, &tx_stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare transaction removal statement");
        }
        
        sqlite3_bind_int(tx_stmt, 1, min_height);
        rc = sqlite3_step(tx_stmt);
        sqlite3_finalize(tx_stmt);
        
        if (rc != SQLITE_DONE) {
            throw std::runtime_error("Failed to remove wallet transactions");
        }
        
        success = true;
        
    } catch (const std::exception& e) {
        g_logger.error("Error in remove_wallet_transactions_from_height: " + std::string(e.what()));
    }
    
    // Commit or rollback
    if (success) {
        rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            g_logger.error("Failed to commit removal transaction: " + std::string(err_msg ? err_msg : "unknown"));
            if (err_msg) sqlite3_free(err_msg);
            success = false;
        }
    } else {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    
    return success;
}

} // namespace dinero
