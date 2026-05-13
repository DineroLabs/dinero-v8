/**
 * Pool Database Implementation
 *
 * SQLite-backed storage for mining pool accounting.
 */

#include "pool/pool_db.h"
#include "common/logger.h"
#include <sqlite3.h>
#include <algorithm>
#include <sstream>
#include <ctime>

namespace dinero {
namespace pool {

namespace {

bool TableHasColumn(sqlite3* db, const std::string& table, const std::string& column) {
    if (!db) {
        return false;
    }

    const std::string pragma = "PRAGMA table_info(" + table + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && column == name) {
            found = true;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

bool EnsureColumn(sqlite3* db, const std::string& table, const std::string& column, const std::string& alter_sql) {
    if (TableHasColumn(db, table, column)) {
        return true;
    }

    char* err_msg = nullptr;
    const int rc = sqlite3_exec(db, alter_sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.error("[PoolDB] Failed to add column " + table + "." + column + ": " +
                       (err_msg ? std::string(err_msg) : "unknown error"));
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        return false;
    }
    if (err_msg) {
        sqlite3_free(err_msg);
    }
    return true;
}

} // namespace

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PoolDB::PoolDB(const std::string& db_path)
    : db_path_(db_path), db_(nullptr) {
}

PoolDB::~PoolDB() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ============================================================================
// INITIALIZATION
// ============================================================================

bool PoolDB::initialize() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        g_logger.error("[PoolDB] Failed to open database: " + db_path_);
        return false;
    }

    // Enable WAL mode for better concurrency
    executeSQL("PRAGMA journal_mode=WAL;");
    executeSQL("PRAGMA synchronous=NORMAL;");
    executeSQL("PRAGMA foreign_keys=ON;");

    if (!createTables()) {
        g_logger.error("[PoolDB] Failed to create tables");
        return false;
    }

    if (!createIndexes()) {
        g_logger.error("[PoolDB] Failed to create indexes");
        return false;
    }

    // Schema migrations for pool payout lifecycle metadata.
    if (!EnsureColumn(db_, "payouts", "retry_count",
                      "ALTER TABLE payouts ADD COLUMN retry_count INTEGER DEFAULT 0;")) {
        return false;
    }
    if (!EnsureColumn(db_, "payouts", "last_retry_at",
                      "ALTER TABLE payouts ADD COLUMN last_retry_at INTEGER DEFAULT 0;")) {
        return false;
    }

    g_logger.info("[PoolDB] Database initialized: " + db_path_);
    return true;
}

bool PoolDB::createTables() {
    // Shares table
    const char* shares_sql = R"(
        CREATE TABLE IF NOT EXISTS shares (
            share_id INTEGER PRIMARY KEY AUTOINCREMENT,
            worker_id TEXT NOT NULL,
            wallet_address TEXT NOT NULL,
            job_id TEXT NOT NULL,
            difficulty INTEGER NOT NULL,
            difficulty_real REAL NOT NULL,
            status INTEGER NOT NULL,
            block_hash TEXT,
            block_height INTEGER,
            block_reward INTEGER,
            submitted_at INTEGER NOT NULL
        );
    )";

    // Share dedupe table
    const char* share_dedupe_sql = R"(
        CREATE TABLE IF NOT EXISTS share_dedupe (
            dedupe_key TEXT PRIMARY KEY,
            worker_id TEXT NOT NULL,
            submitted_at INTEGER NOT NULL
        );
    )";

    // Workers table
    const char* workers_sql = R"(
        CREATE TABLE IF NOT EXISTS workers (
            worker_id TEXT PRIMARY KEY,
            wallet_address TEXT NOT NULL,
            shares_valid INTEGER DEFAULT 0,
            shares_stale INTEGER DEFAULT 0,
            shares_invalid INTEGER DEFAULT 0,
            blocks_found INTEGER DEFAULT 0,
            current_difficulty REAL DEFAULT 1.0,
            total_difficulty REAL DEFAULT 0.0,
            hashrate_1m REAL DEFAULT 0.0,
            hashrate_15m REAL DEFAULT 0.0,
            hashrate_1h REAL DEFAULT 0.0,
            hashrate_24h REAL DEFAULT 0.0,
            total_earned INTEGER DEFAULT 0,
            pending_payout INTEGER DEFAULT 0,
            total_paid INTEGER DEFAULT 0,
            first_seen INTEGER,
            last_seen INTEGER,
            last_share INTEGER
        );
    )";

    // Blocks table
    const char* blocks_sql = R"(
        CREATE TABLE IF NOT EXISTS blocks (
            block_id INTEGER PRIMARY KEY AUTOINCREMENT,
            block_hash TEXT UNIQUE NOT NULL,
            height INTEGER NOT NULL,
            finder_worker TEXT NOT NULL,
            finder_address TEXT NOT NULL,
            reward INTEGER NOT NULL,
            fees INTEGER DEFAULT 0,
            total_reward INTEGER NOT NULL,
            pool_fee_percent REAL NOT NULL,
            pool_fee_amount INTEGER NOT NULL,
            distributable INTEGER NOT NULL,
            round_shares INTEGER DEFAULT 0,
            round_difficulty REAL DEFAULT 0.0,
            confirmations INTEGER DEFAULT 0,
            required_confirmations INTEGER DEFAULT 100,
            orphaned INTEGER DEFAULT 0,
            payouts_calculated INTEGER DEFAULT 0,
            payouts_sent INTEGER DEFAULT 0,
            found_at INTEGER NOT NULL,
            confirmed_at INTEGER DEFAULT 0
        );
    )";

    // Payouts table
    const char* payouts_sql = R"(
        CREATE TABLE IF NOT EXISTS payouts (
            payout_id INTEGER PRIMARY KEY AUTOINCREMENT,
            block_id INTEGER,
            worker_id TEXT NOT NULL,
            wallet_address TEXT NOT NULL,
            amount INTEGER NOT NULL,
            share_percent REAL NOT NULL,
            share_count INTEGER NOT NULL,
            difficulty_sum REAL NOT NULL,
            status INTEGER NOT NULL,
            txid TEXT,
            error_message TEXT,
            calculated_at INTEGER NOT NULL,
            paid_at INTEGER DEFAULT 0,
            retry_count INTEGER DEFAULT 0,
            last_retry_at INTEGER DEFAULT 0,
            FOREIGN KEY (block_id) REFERENCES blocks(block_id)
        );
    )";

    // Rounds table (for PROP mode)
    const char* rounds_sql = R"(
        CREATE TABLE IF NOT EXISTS rounds (
            round_id INTEGER PRIMARY KEY AUTOINCREMENT,
            block_id INTEGER DEFAULT 0,
            total_shares INTEGER DEFAULT 0,
            total_difficulty REAL DEFAULT 0.0,
            started_at INTEGER NOT NULL,
            ended_at INTEGER DEFAULT 0
        );
    )";

    // Round shares (worker difficulty per round)
    const char* round_shares_sql = R"(
        CREATE TABLE IF NOT EXISTS round_shares (
            round_id INTEGER NOT NULL,
            worker_id TEXT NOT NULL,
            difficulty_sum REAL DEFAULT 0.0,
            share_count INTEGER DEFAULT 0,
            PRIMARY KEY (round_id, worker_id),
            FOREIGN KEY (round_id) REFERENCES rounds(round_id)
        );
    )";

    // Config table
    const char* config_sql = R"(
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )";

    return executeSQL(shares_sql) &&
           executeSQL(share_dedupe_sql) &&
           executeSQL(workers_sql) &&
           executeSQL(blocks_sql) &&
           executeSQL(payouts_sql) &&
           executeSQL(rounds_sql) &&
           executeSQL(round_shares_sql) &&
           executeSQL(config_sql);
}

bool PoolDB::createIndexes() {
    // Shares indexes
    executeSQL("CREATE INDEX IF NOT EXISTS idx_shares_worker ON shares(worker_id);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_shares_time ON shares(submitted_at);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_shares_status ON shares(status);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_shares_job ON shares(job_id);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_share_dedupe_time ON share_dedupe(submitted_at);");

    // Workers indexes
    executeSQL("CREATE INDEX IF NOT EXISTS idx_workers_address ON workers(wallet_address);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_workers_pending ON workers(pending_payout);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_workers_last_share ON workers(last_share);");

    // Blocks indexes
    executeSQL("CREATE INDEX IF NOT EXISTS idx_blocks_height ON blocks(height);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_blocks_orphaned ON blocks(orphaned);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_blocks_confirmations ON blocks(confirmations);");

    // Payouts indexes
    executeSQL("CREATE INDEX IF NOT EXISTS idx_payouts_worker ON payouts(worker_id);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_payouts_block ON payouts(block_id);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_payouts_status ON payouts(status);");

    return true;
}

// ============================================================================
// SQL HELPERS
// ============================================================================

bool PoolDB::executeSQL(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.error("[PoolDB] SQL error: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool PoolDB::executeSQL(const std::string& sql, std::function<void(sqlite3_stmt*)> bind_fn) {
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        g_logger.error("[PoolDB] Prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    bind_fn(stmt);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

// ============================================================================
// SHARE OPERATIONS
// ============================================================================

bool PoolDB::insertShare(const Share& share) {
    const char* sql = R"(
        INSERT INTO shares (worker_id, wallet_address, job_id, difficulty,
                           difficulty_real, status, block_hash, block_height,
                           block_reward, submitted_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    return executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_text(stmt, 1, share.worker_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, share.wallet_address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, share.job_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, share.difficulty);
        sqlite3_bind_double(stmt, 5, share.difficulty_real);
        sqlite3_bind_int(stmt, 6, static_cast<int>(share.status));
        sqlite3_bind_text(stmt, 7, share.block_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, share.block_height);
        sqlite3_bind_int64(stmt, 9, share.block_reward);
        sqlite3_bind_int64(stmt, 10, share.submitted_at);
    });
}

bool PoolDB::runInTransaction(const std::function<bool()>& fn) {
    if (!db_) {
        g_logger.error("[PoolDB] Cannot start transaction: database is not open");
        return false;
    }

    if (!executeSQL("BEGIN IMMEDIATE;")) {
        g_logger.error("[PoolDB] Failed to begin transaction");
        return false;
    }

    auto rollback = [&]() {
        if (sqlite3_get_autocommit(db_) == 0 && !executeSQL("ROLLBACK;")) {
            g_logger.error("[PoolDB] Failed to roll back transaction");
        }
    };

    try {
        if (!fn()) {
            rollback();
            return false;
        }
    } catch (const std::exception& e) {
        g_logger.error(std::string("[PoolDB] Transaction aborted by exception: ") + e.what());
        rollback();
        return false;
    } catch (...) {
        g_logger.error("[PoolDB] Transaction aborted by unknown exception");
        rollback();
        return false;
    }

    if (!executeSQL("COMMIT;")) {
        g_logger.error("[PoolDB] Failed to commit transaction");
        rollback();
        return false;
    }

    return true;
}

PoolDB::ShareSubmissionReservationResult PoolDB::reserveShareSubmissionKey(const std::string& dedupe_key,
                                                                           const std::string& worker_id,
                                                                           int64_t submitted_at) {
    if (dedupe_key.empty()) {
        g_logger.error("[PoolDB] Cannot reserve empty share dedupe key");
        return ShareSubmissionReservationResult::Error;
    }

    const char* sql = R"(
        INSERT OR IGNORE INTO share_dedupe (dedupe_key, worker_id, submitted_at)
        VALUES (?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error(std::string("[PoolDB] Failed to prepare share dedupe reservation: ") +
                       sqlite3_errmsg(db_));
        return ShareSubmissionReservationResult::Error;
    }

    sqlite3_bind_text(stmt, 1, dedupe_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, submitted_at);

    const int rc = sqlite3_step(stmt);
    const int rows_changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        g_logger.error(std::string("[PoolDB] Failed to reserve share dedupe key: ") +
                       sqlite3_errmsg(db_));
        return ShareSubmissionReservationResult::Error;
    }

    if (rows_changed > 0) {
        return ShareSubmissionReservationResult::Reserved;
    }

    return ShareSubmissionReservationResult::Duplicate;
}

uint64_t PoolDB::pruneShareSubmissionKeysOlderThan(int64_t cutoff_timestamp) {
    const char* sql = "DELETE FROM share_dedupe WHERE submitted_at < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, cutoff_timestamp);
    const int rc = sqlite3_step(stmt);
    const int rows_changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return 0;
    }
    return static_cast<uint64_t>(std::max(0, rows_changed));
}

std::vector<Share> PoolDB::getLastNShares(uint64_t n) {
    std::vector<Share> shares;

    const char* sql = R"(
        SELECT share_id, worker_id, wallet_address, job_id, difficulty,
               difficulty_real, status, block_hash, block_height,
               block_reward, submitted_at
        FROM shares
        WHERE status = 0
        ORDER BY share_id DESC
        LIMIT ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return shares;
    }

    sqlite3_bind_int64(stmt, 1, n);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Share s;
        s.share_id = sqlite3_column_int64(stmt, 0);
        s.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        s.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.job_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        s.difficulty = sqlite3_column_int(stmt, 4);
        s.difficulty_real = sqlite3_column_double(stmt, 5);
        s.status = static_cast<ShareStatus>(sqlite3_column_int(stmt, 6));
        const char* bh = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (bh) s.block_hash = bh;
        s.block_height = sqlite3_column_int(stmt, 8);
        s.block_reward = sqlite3_column_int64(stmt, 9);
        s.submitted_at = sqlite3_column_int64(stmt, 10);
        shares.push_back(s);
    }

    sqlite3_finalize(stmt);
    return shares;
}

std::vector<Share> PoolDB::getWorkerShares(const std::string& worker_id,
                                           uint32_t limit) {
    std::vector<Share> shares;

    const char* sql = R"(
        SELECT share_id, worker_id, wallet_address, job_id, difficulty,
               difficulty_real, status, block_hash, block_height,
               block_reward, submitted_at
        FROM shares
        WHERE worker_id = ?
        ORDER BY submitted_at DESC
        LIMIT ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return shares;
    }

    sqlite3_bind_text(stmt, 1, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Share s;
        s.share_id = sqlite3_column_int64(stmt, 0);
        s.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        s.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.job_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        s.difficulty = sqlite3_column_int(stmt, 4);
        s.difficulty_real = sqlite3_column_double(stmt, 5);
        s.status = static_cast<ShareStatus>(sqlite3_column_int(stmt, 6));
        const char* bh = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (bh) s.block_hash = bh;
        s.block_height = sqlite3_column_int(stmt, 8);
        s.block_reward = sqlite3_column_int64(stmt, 9);
        s.submitted_at = sqlite3_column_int64(stmt, 10);
        shares.push_back(s);
    }

    sqlite3_finalize(stmt);
    return shares;
}

double PoolDB::getWorkerDifficultyInRange(const std::string& worker_id,
                                           int64_t start_time, int64_t end_time) {
    const char* sql = R"(
        SELECT COALESCE(SUM(difficulty_real), 0.0)
        FROM shares
        WHERE worker_id = ? AND status = 0
        AND submitted_at >= ? AND submitted_at <= ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0.0;
    }

    sqlite3_bind_text(stmt, 1, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, start_time);
    sqlite3_bind_int64(stmt, 3, end_time);

    double result = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_double(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return result;
}

// ============================================================================
// WORKER OPERATIONS
// ============================================================================

WorkerStats PoolDB::getOrCreateWorker(const std::string& worker_id,
                                       const std::string& wallet_address) {
    // Try to get existing
    auto existing = getWorker(worker_id);
    if (existing) {
        return *existing;
    }

    // Create new worker
    int64_t now = std::time(nullptr);
    const char* sql = R"(
        INSERT INTO workers (worker_id, wallet_address, first_seen, last_seen)
        VALUES (?, ?, ?, ?);
    )";

    executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_text(stmt, 1, worker_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, wallet_address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_int64(stmt, 4, now);
    });

    WorkerStats stats;
    stats.worker_id = worker_id;
    stats.wallet_address = wallet_address;
    stats.first_seen = now;
    stats.last_seen = now;
    return stats;
}

std::optional<WorkerStats> PoolDB::getWorker(const std::string& worker_id) {
    const char* sql = R"(
        SELECT worker_id, wallet_address, shares_valid, shares_stale,
               shares_invalid, blocks_found, current_difficulty, total_difficulty,
               hashrate_1m, hashrate_15m, hashrate_1h, hashrate_24h,
               total_earned, pending_payout, total_paid, first_seen, last_seen, last_share
        FROM workers
        WHERE worker_id = ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, worker_id.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<WorkerStats> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkerStats w;
        w.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        w.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        w.shares_valid = sqlite3_column_int64(stmt, 2);
        w.shares_stale = sqlite3_column_int64(stmt, 3);
        w.shares_invalid = sqlite3_column_int64(stmt, 4);
        w.blocks_found = sqlite3_column_int64(stmt, 5);
        w.current_difficulty = sqlite3_column_double(stmt, 6);
        w.total_difficulty = sqlite3_column_double(stmt, 7);
        w.hashrate_1m = sqlite3_column_double(stmt, 8);
        w.hashrate_15m = sqlite3_column_double(stmt, 9);
        w.hashrate_1h = sqlite3_column_double(stmt, 10);
        w.hashrate_24h = sqlite3_column_double(stmt, 11);
        w.total_earned = sqlite3_column_int64(stmt, 12);
        w.pending_payout = sqlite3_column_int64(stmt, 13);
        w.total_paid = sqlite3_column_int64(stmt, 14);
        w.first_seen = sqlite3_column_int64(stmt, 15);
        w.last_seen = sqlite3_column_int64(stmt, 16);
        w.last_share = sqlite3_column_int64(stmt, 17);
        result = w;
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<WorkerStats> PoolDB::getWorkersByAddress(const std::string& wallet_address) {
    std::vector<WorkerStats> workers;

    const char* sql = R"(
        SELECT worker_id, wallet_address, shares_valid, shares_stale,
               shares_invalid, blocks_found, current_difficulty, total_difficulty,
               hashrate_1m, hashrate_15m, hashrate_1h, hashrate_24h,
               total_earned, pending_payout, total_paid, first_seen, last_seen, last_share
        FROM workers
        WHERE wallet_address = ?
        ORDER BY pending_payout DESC, last_share DESC;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return workers;
    }

    sqlite3_bind_text(stmt, 1, wallet_address.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkerStats w;
        w.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        w.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        w.shares_valid = sqlite3_column_int64(stmt, 2);
        w.shares_stale = sqlite3_column_int64(stmt, 3);
        w.shares_invalid = sqlite3_column_int64(stmt, 4);
        w.blocks_found = sqlite3_column_int64(stmt, 5);
        w.current_difficulty = sqlite3_column_double(stmt, 6);
        w.total_difficulty = sqlite3_column_double(stmt, 7);
        w.hashrate_1m = sqlite3_column_double(stmt, 8);
        w.hashrate_15m = sqlite3_column_double(stmt, 9);
        w.hashrate_1h = sqlite3_column_double(stmt, 10);
        w.hashrate_24h = sqlite3_column_double(stmt, 11);
        w.total_earned = sqlite3_column_int64(stmt, 12);
        w.pending_payout = sqlite3_column_int64(stmt, 13);
        w.total_paid = sqlite3_column_int64(stmt, 14);
        w.first_seen = sqlite3_column_int64(stmt, 15);
        w.last_seen = sqlite3_column_int64(stmt, 16);
        w.last_share = sqlite3_column_int64(stmt, 17);
        workers.push_back(w);
    }

    sqlite3_finalize(stmt);
    return workers;
}

bool PoolDB::updateWorkerStats(const WorkerStats& stats) {
    const char* sql = R"(
        UPDATE workers SET
            shares_valid = ?, shares_stale = ?, shares_invalid = ?,
            blocks_found = ?, current_difficulty = ?, total_difficulty = ?,
            hashrate_1m = ?, hashrate_15m = ?, hashrate_1h = ?, hashrate_24h = ?,
            total_earned = ?, pending_payout = ?, total_paid = ?,
            last_seen = ?, last_share = ?
        WHERE worker_id = ?;
    )";

    return executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_int64(stmt, 1, stats.shares_valid);
        sqlite3_bind_int64(stmt, 2, stats.shares_stale);
        sqlite3_bind_int64(stmt, 3, stats.shares_invalid);
        sqlite3_bind_int64(stmt, 4, stats.blocks_found);
        sqlite3_bind_double(stmt, 5, stats.current_difficulty);
        sqlite3_bind_double(stmt, 6, stats.total_difficulty);
        sqlite3_bind_double(stmt, 7, stats.hashrate_1m);
        sqlite3_bind_double(stmt, 8, stats.hashrate_15m);
        sqlite3_bind_double(stmt, 9, stats.hashrate_1h);
        sqlite3_bind_double(stmt, 10, stats.hashrate_24h);
        sqlite3_bind_int64(stmt, 11, stats.total_earned);
        sqlite3_bind_int64(stmt, 12, stats.pending_payout);
        sqlite3_bind_int64(stmt, 13, stats.total_paid);
        sqlite3_bind_int64(stmt, 14, stats.last_seen);
        sqlite3_bind_int64(stmt, 15, stats.last_share);
        sqlite3_bind_text(stmt, 16, stats.worker_id.c_str(), -1, SQLITE_TRANSIENT);
    });
}

std::vector<WorkerStats> PoolDB::getActiveWorkers(int64_t seconds) {
    std::vector<WorkerStats> workers;
    int64_t cutoff = std::time(nullptr) - seconds;

    const char* sql = R"(
        SELECT worker_id, wallet_address, shares_valid, shares_stale,
               shares_invalid, blocks_found, current_difficulty, total_difficulty,
               hashrate_1m, hashrate_15m, hashrate_1h, hashrate_24h,
               total_earned, pending_payout, total_paid, first_seen, last_seen, last_share
        FROM workers
        WHERE last_share >= ?
        ORDER BY hashrate_15m DESC;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return workers;
    }

    sqlite3_bind_int64(stmt, 1, cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkerStats w;
        w.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        w.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        w.shares_valid = sqlite3_column_int64(stmt, 2);
        w.shares_stale = sqlite3_column_int64(stmt, 3);
        w.shares_invalid = sqlite3_column_int64(stmt, 4);
        w.blocks_found = sqlite3_column_int64(stmt, 5);
        w.current_difficulty = sqlite3_column_double(stmt, 6);
        w.total_difficulty = sqlite3_column_double(stmt, 7);
        w.hashrate_1m = sqlite3_column_double(stmt, 8);
        w.hashrate_15m = sqlite3_column_double(stmt, 9);
        w.hashrate_1h = sqlite3_column_double(stmt, 10);
        w.hashrate_24h = sqlite3_column_double(stmt, 11);
        w.total_earned = sqlite3_column_int64(stmt, 12);
        w.pending_payout = sqlite3_column_int64(stmt, 13);
        w.total_paid = sqlite3_column_int64(stmt, 14);
        w.first_seen = sqlite3_column_int64(stmt, 15);
        w.last_seen = sqlite3_column_int64(stmt, 16);
        w.last_share = sqlite3_column_int64(stmt, 17);
        workers.push_back(w);
    }

    sqlite3_finalize(stmt);
    return workers;
}

bool PoolDB::addWorkerPending(const std::string& worker_id, uint64_t amount) {
    const char* sql = R"(
        UPDATE workers SET
            pending_payout = pending_payout + ?,
            total_earned = total_earned + ?
        WHERE worker_id = ?;
    )";

    return executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_int64(stmt, 1, amount);
        sqlite3_bind_int64(stmt, 2, amount);
        sqlite3_bind_text(stmt, 3, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    });
}

// ============================================================================
// BLOCK OPERATIONS
// ============================================================================

bool PoolDB::insertBlock(PoolBlock& block) {
    const char* sql = R"(
        INSERT INTO blocks (block_hash, height, finder_worker, finder_address,
                           reward, fees, total_reward, pool_fee_percent,
                           pool_fee_amount, distributable, round_shares,
                           round_difficulty, confirmations, required_confirmations,
                           orphaned, payouts_calculated, payouts_sent, found_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    const bool inserted = executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_text(stmt, 1, block.block_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, block.height);
        sqlite3_bind_text(stmt, 3, block.finder_worker.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, block.finder_address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, block.reward);
        sqlite3_bind_int64(stmt, 6, block.fees);
        sqlite3_bind_int64(stmt, 7, block.total_reward);
        sqlite3_bind_double(stmt, 8, block.pool_fee_percent);
        sqlite3_bind_int64(stmt, 9, block.pool_fee_amount);
        sqlite3_bind_int64(stmt, 10, block.distributable);
        sqlite3_bind_int64(stmt, 11, block.round_shares);
        sqlite3_bind_double(stmt, 12, block.round_difficulty);
        sqlite3_bind_int(stmt, 13, block.confirmations);
        sqlite3_bind_int(stmt, 14, block.required_confirmations);
        sqlite3_bind_int(stmt, 15, block.orphaned ? 1 : 0);
        sqlite3_bind_int(stmt, 16, block.payouts_calculated ? 1 : 0);
        sqlite3_bind_int(stmt, 17, block.payouts_sent ? 1 : 0);
        sqlite3_bind_int64(stmt, 18, block.found_at);
    });

    if (inserted) {
        block.block_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
    }
    return inserted;
}

std::vector<PoolBlock> PoolDB::getPendingBlocks() {
    std::vector<PoolBlock> blocks;

    const char* sql = R"(
        SELECT block_id, block_hash, height, finder_worker, finder_address,
               reward, fees, total_reward, pool_fee_percent, pool_fee_amount,
               distributable, round_shares, round_difficulty, confirmations,
               required_confirmations, orphaned, payouts_calculated, payouts_sent,
               found_at, confirmed_at
        FROM blocks
        WHERE orphaned = 0 AND confirmations < required_confirmations
        ORDER BY height DESC;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return blocks;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PoolBlock b;
        b.block_id = sqlite3_column_int64(stmt, 0);
        b.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        b.height = sqlite3_column_int(stmt, 2);
        b.finder_worker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        b.finder_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        b.reward = sqlite3_column_int64(stmt, 5);
        b.fees = sqlite3_column_int64(stmt, 6);
        b.total_reward = sqlite3_column_int64(stmt, 7);
        b.pool_fee_percent = sqlite3_column_double(stmt, 8);
        b.pool_fee_amount = sqlite3_column_int64(stmt, 9);
        b.distributable = sqlite3_column_int64(stmt, 10);
        b.round_shares = sqlite3_column_int64(stmt, 11);
        b.round_difficulty = sqlite3_column_double(stmt, 12);
        b.confirmations = sqlite3_column_int(stmt, 13);
        b.required_confirmations = sqlite3_column_int(stmt, 14);
        b.orphaned = sqlite3_column_int(stmt, 15) != 0;
        b.payouts_calculated = sqlite3_column_int(stmt, 16) != 0;
        b.payouts_sent = sqlite3_column_int(stmt, 17) != 0;
        b.found_at = sqlite3_column_int64(stmt, 18);
        b.confirmed_at = sqlite3_column_int64(stmt, 19);
        blocks.push_back(b);
    }

    sqlite3_finalize(stmt);
    return blocks;
}

// ============================================================================
// PAYOUT OPERATIONS
// ============================================================================

bool PoolDB::insertPayout(const Payout& payout) {
    const char* sql = R"(
        INSERT INTO payouts (block_id, worker_id, wallet_address, amount,
                            share_percent, share_count, difficulty_sum,
                            status, calculated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    return executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_int64(stmt, 1, payout.block_id);
        sqlite3_bind_text(stmt, 2, payout.worker_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, payout.wallet_address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, payout.amount);
        sqlite3_bind_double(stmt, 5, payout.share_percent);
        sqlite3_bind_int64(stmt, 6, payout.share_count);
        sqlite3_bind_double(stmt, 7, payout.difficulty_sum);
        sqlite3_bind_int(stmt, 8, static_cast<int>(payout.status));
        sqlite3_bind_int64(stmt, 9, payout.calculated_at);
    });
}

// ============================================================================
// ROUND OPERATIONS
// ============================================================================

uint64_t PoolDB::startNewRound() {
    int64_t now = std::time(nullptr);
    const char* sql = "INSERT INTO rounds (started_at) VALUES (?);";

    executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_int64(stmt, 1, now);
    });

    return static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));
}

bool PoolDB::endRound(uint64_t round_id, uint64_t block_id) {
    int64_t now = std::time(nullptr);
    const char* sql = R"(
        UPDATE rounds SET block_id = ?, ended_at = ?
        WHERE round_id = ?;
    )";

    return executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_int64(stmt, 1, block_id);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_int64(stmt, 3, round_id);
    });
}

bool PoolDB::addWorkerDifficultyToRound(uint64_t round_id,
                                         const std::string& worker_id,
                                         double difficulty) {
    // Upsert: insert or update if exists
    const char* sql = R"(
        INSERT INTO round_shares (round_id, worker_id, difficulty_sum, share_count)
        VALUES (?, ?, ?, 1)
        ON CONFLICT(round_id, worker_id) DO UPDATE SET
            difficulty_sum = difficulty_sum + excluded.difficulty_sum,
            share_count = share_count + 1;
    )";

    bool result = executeSQL(sql, [&](sqlite3_stmt* stmt) {
        sqlite3_bind_int64(stmt, 1, round_id);
        sqlite3_bind_text(stmt, 2, worker_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, difficulty);
    });

    // Also update round totals
    if (result) {
        const char* update_sql = R"(
            UPDATE rounds SET
                total_shares = total_shares + 1,
                total_difficulty = total_difficulty + ?
            WHERE round_id = ?;
        )";

        executeSQL(update_sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_double(stmt, 1, difficulty);
            sqlite3_bind_int64(stmt, 2, round_id);
        });
    }

    return result;
}

uint64_t PoolDB::countWorkerSharesInRound(const std::string& worker_id,
                                          uint64_t round_id) {
    const char* sql = R"(
        SELECT COALESCE(share_count, 0)
        FROM round_shares
        WHERE round_id = ? AND worker_id = ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(round_id));
    sqlite3_bind_text(stmt, 2, worker_id.c_str(), -1, SQLITE_TRANSIENT);

    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

std::optional<MiningRound> PoolDB::getCurrentRound() {
    const char* sql = R"(
        SELECT round_id, block_id, total_shares, total_difficulty, started_at, ended_at
        FROM rounds
        WHERE ended_at = 0
        ORDER BY round_id DESC
        LIMIT 1;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    std::optional<MiningRound> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        MiningRound r;
        r.round_id = sqlite3_column_int64(stmt, 0);
        r.block_id = sqlite3_column_int64(stmt, 1);
        r.total_shares = sqlite3_column_int64(stmt, 2);
        r.total_difficulty = sqlite3_column_double(stmt, 3);
        r.started_at = sqlite3_column_int64(stmt, 4);
        r.ended_at = sqlite3_column_int64(stmt, 5);
        result = r;
    }

    sqlite3_finalize(stmt);
    return result;
}

// ============================================================================
// POOL STATS
// ============================================================================

PoolStats PoolDB::getPoolStats() {
    PoolStats stats;
    const int64_t now = std::time(nullptr);

    // Active workers (last 15 min)
    {
        int64_t cutoff = now - 900;
        const char* sql = "SELECT COUNT(*) FROM workers WHERE last_share >= ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, cutoff);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.active_workers = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Total workers
    {
        const char* sql = "SELECT COUNT(*) FROM workers;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.total_workers = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Pool hashrate (sum of active workers)
    {
        int64_t cutoff = now - 900;
        const char* sql = "SELECT COALESCE(SUM(hashrate_15m), 0) FROM workers WHERE last_share >= ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, cutoff);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.pool_hashrate = sqlite3_column_double(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Blocks found
    {
        const char* sql = "SELECT COUNT(*) FROM blocks WHERE orphaned = 0;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.blocks_found = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Orphaned blocks
    {
        const char* sql = "SELECT COUNT(*) FROM blocks WHERE orphaned = 1;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.blocks_orphaned = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Pending blocks (found but not yet mature)
    {
        const char* sql = R"(
            SELECT COUNT(*)
            FROM blocks
            WHERE orphaned = 0
              AND confirmations < required_confirmations;
        )";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.blocks_pending = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Total paid
    {
        const char* sql = "SELECT COALESCE(SUM(total_paid), 0) FROM workers;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.total_paid = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Pending payouts
    {
        const char* sql = "SELECT COALESCE(SUM(pending_payout), 0) FROM workers;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.pending_payouts = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Share throughput
    stats.total_shares = getTotalSharesInPeriod(0, now);
    const uint64_t shares_15m = getTotalSharesInPeriod(now - 900, now);
    stats.shares_per_second = shares_15m / 900;

    // Current round
    auto round = getCurrentRound();
    if (round) {
        stats.round_shares = round->total_shares;
        stats.round_start = round->started_at;
    }

    // Luck snapshots
    stats.luck_1d = calculateLuck(24 * 60 * 60);
    stats.luck_7d = calculateLuck(7 * 24 * 60 * 60);
    stats.luck_30d = calculateLuck(30 * 24 * 60 * 60);

    return stats;
}

uint64_t PoolDB::getTotalSharesInPeriod(int64_t start_time, int64_t end_time) {
    const char* sql = R"(
        SELECT COUNT(*)
        FROM shares
        WHERE submitted_at >= ? AND submitted_at <= ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, start_time);
    sqlite3_bind_int64(stmt, 2, end_time);

    uint64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return total;
}

double PoolDB::getTotalDifficultyInPeriod(int64_t start_time, int64_t end_time) {
    const char* sql = R"(
        SELECT COALESCE(SUM(difficulty_real), 0.0)
        FROM shares
        WHERE status IN (0, 4)
          AND submitted_at >= ? AND submitted_at <= ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0.0;
    }
    sqlite3_bind_int64(stmt, 1, start_time);
    sqlite3_bind_int64(stmt, 2, end_time);

    double total = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return total;
}

double PoolDB::calculateLuck(int64_t period_seconds) {
    if (period_seconds <= 0) {
        return 0.0;
    }

    const int64_t now = std::time(nullptr);
    const int64_t start_time = now - period_seconds;

    // Actual blocks found in period (excluding orphans)
    uint64_t actual_blocks = 0;
    {
        const char* sql = R"(
            SELECT COUNT(*)
            FROM blocks
            WHERE orphaned = 0 AND found_at >= ?;
        )";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return 0.0;
        }
        sqlite3_bind_int64(stmt, 1, start_time);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            actual_blocks = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    const uint64_t period_shares = getTotalSharesInPeriod(start_time, now);
    if (period_shares == 0) {
        return 0.0;
    }

    // Use historical pool baseline shares-per-block to estimate expected blocks.
    uint64_t historical_blocks = 0;
    {
        const char* sql = R"(
            SELECT COUNT(*)
            FROM blocks
            WHERE orphaned = 0;
        )";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return 0.0;
        }
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            historical_blocks = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    if (historical_blocks == 0) {
        return 0.0;
    }

    const uint64_t historical_shares = getTotalSharesInPeriod(0, now);
    if (historical_shares == 0) {
        return 0.0;
    }

    const double shares_per_block =
        static_cast<double>(historical_shares) / static_cast<double>(historical_blocks);
    if (shares_per_block <= 0.0) {
        return 0.0;
    }

    const double expected_blocks = static_cast<double>(period_shares) / shares_per_block;
    if (expected_blocks <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(actual_blocks) / expected_blocks;
}

// ============================================================================
// CONFIG
// ============================================================================

PoolConfig PoolDB::getConfig() {
    PoolConfig config;

    const char* sql = "SELECT key, value FROM config;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return config;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (key == "payout_mode") config.payout_mode = StringToPayoutMode(value);
        else if (key == "pplns_window") config.pplns_window = std::stoull(value);
        else if (key == "pps_rate") config.pps_rate = std::stod(value);
        else if (key == "pool_fee_percent") config.pool_fee_percent = std::stod(value);
        else if (key == "pool_fee_address") config.pool_fee_address = value;
        else if (key == "min_payout") config.min_payout = std::stoull(value);
        else if (key == "min_auto_payout") config.min_auto_payout = std::stoull(value);
        else if (key == "max_payout_retries") config.max_payout_retries = std::stoul(value);
        else if (key == "required_confirmations") config.required_confirmations = std::stoul(value);
        else if (key == "new_round_on_block") {
            config.new_round_on_block =
                (value == "1" || value == "true" || value == "TRUE");
        }
    }

    sqlite3_finalize(stmt);
    return config;
}

bool PoolDB::updateConfig(const PoolConfig& config) {
    // Simple key-value upserts
    auto upsert = [this](const std::string& key, const std::string& value) {
        const char* sql = R"(
            INSERT INTO config (key, value) VALUES (?, ?)
            ON CONFLICT(key) DO UPDATE SET value = excluded.value;
        )";
        return executeSQL(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        });
    };

    bool ok = true;
    ok = upsert("payout_mode", PayoutModeToString(config.payout_mode)) && ok;
    ok = upsert("pplns_window", std::to_string(config.pplns_window)) && ok;
    ok = upsert("pps_rate", std::to_string(config.pps_rate)) && ok;
    ok = upsert("pool_fee_percent", std::to_string(config.pool_fee_percent)) && ok;
    ok = upsert("pool_fee_address", config.pool_fee_address) && ok;
    ok = upsert("min_payout", std::to_string(config.min_payout)) && ok;
    ok = upsert("min_auto_payout", std::to_string(config.min_auto_payout)) && ok;
    ok = upsert("max_payout_retries", std::to_string(config.max_payout_retries)) && ok;
    ok = upsert("required_confirmations", std::to_string(config.required_confirmations)) && ok;
    ok = upsert("new_round_on_block", config.new_round_on_block ? "1" : "0") && ok;

    return ok;
}

// ============================================================================
// MAINTENANCE
// ============================================================================

uint64_t PoolDB::pruneOldShares(int64_t days) {
    int64_t cutoff = std::time(nullptr) - (days * 86400);

    const char* sql = "DELETE FROM shares WHERE submitted_at < ? AND status != 4;"; // Keep block shares

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, cutoff);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return static_cast<uint64_t>(sqlite3_changes(db_));
}

bool PoolDB::vacuum() {
    return executeSQL("VACUUM;");
}

uint64_t PoolDB::getDatabaseSize() {
    const char* sql = "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size();";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    uint64_t size = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        size = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return size;
}

// ============================================================================
// ADDITIONAL METHODS (MISSING IMPLEMENTATIONS)
// ============================================================================

bool PoolDB::updateBlock(const PoolBlock& block) {
    const char* sql = R"(
        UPDATE blocks SET
            confirmations = ?,
            orphaned = ?,
            payouts_calculated = ?,
            payouts_sent = ?,
            confirmed_at = ?
        WHERE block_id = ?
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, block.confirmations);
    sqlite3_bind_int(stmt, 2, block.orphaned ? 1 : 0);
    sqlite3_bind_int(stmt, 3, block.payouts_calculated ? 1 : 0);
    sqlite3_bind_int(stmt, 4, block.payouts_sent ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, block.confirmed_at);
    sqlite3_bind_int64(stmt, 6, block.block_id);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool PoolDB::addWorkerPaid(const std::string& worker_id, uint64_t amount) {
    const char* sql = "UPDATE workers SET total_paid = total_paid + ? WHERE worker_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, amount);
    sqlite3_bind_text(stmt, 2, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool PoolDB::subtractWorkerPending(const std::string& worker_id, uint64_t amount) {
    const char* sql = R"(
        UPDATE workers
        SET pending_payout = CASE
            WHEN pending_payout >= ? THEN pending_payout - ?
            ELSE 0
        END
        WHERE worker_id = ?
    )";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, amount);
    sqlite3_bind_int64(stmt, 2, amount);
    sqlite3_bind_text(stmt, 3, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

std::optional<PoolBlock> PoolDB::getBlockByHash(const std::string& block_hash) {
    const char* sql = "SELECT * FROM blocks WHERE block_hash = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, block_hash.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<PoolBlock> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        PoolBlock block;
        block.block_id = sqlite3_column_int64(stmt, 0);
        block.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        block.height = sqlite3_column_int(stmt, 2);
        block.finder_worker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        block.finder_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        block.reward = sqlite3_column_int64(stmt, 5);
        block.fees = sqlite3_column_int64(stmt, 6);
        block.total_reward = sqlite3_column_int64(stmt, 7);
        block.pool_fee_percent = sqlite3_column_double(stmt, 8);
        block.pool_fee_amount = sqlite3_column_int64(stmt, 9);
        block.distributable = sqlite3_column_int64(stmt, 10);
        block.round_shares = sqlite3_column_int64(stmt, 11);
        block.round_difficulty = sqlite3_column_double(stmt, 12);
        block.confirmations = sqlite3_column_int(stmt, 13);
        block.required_confirmations = sqlite3_column_int(stmt, 14);
        block.orphaned = sqlite3_column_int(stmt, 15) != 0;
        block.payouts_calculated = sqlite3_column_int(stmt, 16) != 0;
        block.payouts_sent = sqlite3_column_int(stmt, 17) != 0;
        block.found_at = sqlite3_column_int64(stmt, 18);
        block.confirmed_at = sqlite3_column_int64(stmt, 19);
        result = block;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<PoolBlock> PoolDB::getBlock(uint64_t block_id) {
    const char* sql = "SELECT * FROM blocks WHERE block_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(block_id));

    std::optional<PoolBlock> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        PoolBlock block;
        block.block_id = sqlite3_column_int64(stmt, 0);
        block.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        block.height = sqlite3_column_int(stmt, 2);
        block.finder_worker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        block.finder_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        block.reward = sqlite3_column_int64(stmt, 5);
        block.fees = sqlite3_column_int64(stmt, 6);
        block.total_reward = sqlite3_column_int64(stmt, 7);
        block.pool_fee_percent = sqlite3_column_double(stmt, 8);
        block.pool_fee_amount = sqlite3_column_int64(stmt, 9);
        block.distributable = sqlite3_column_int64(stmt, 10);
        block.round_shares = sqlite3_column_int64(stmt, 11);
        block.round_difficulty = sqlite3_column_double(stmt, 12);
        block.confirmations = sqlite3_column_int(stmt, 13);
        block.required_confirmations = sqlite3_column_int(stmt, 14);
        block.orphaned = sqlite3_column_int(stmt, 15) != 0;
        block.payouts_calculated = sqlite3_column_int(stmt, 16) != 0;
        block.payouts_sent = sqlite3_column_int(stmt, 17) != 0;
        block.found_at = sqlite3_column_int64(stmt, 18);
        block.confirmed_at = sqlite3_column_int64(stmt, 19);
        result = block;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<PoolBlock> PoolDB::getRecentBlocks(uint32_t limit) {
    std::vector<PoolBlock> blocks;
    const char* sql = "SELECT * FROM blocks ORDER BY found_at DESC LIMIT ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return blocks;
    }
    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PoolBlock block;
        block.block_id = sqlite3_column_int64(stmt, 0);
        block.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        block.height = sqlite3_column_int(stmt, 2);
        block.finder_worker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        block.finder_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        block.reward = sqlite3_column_int64(stmt, 5);
        block.fees = sqlite3_column_int64(stmt, 6);
        block.total_reward = sqlite3_column_int64(stmt, 7);
        block.pool_fee_percent = sqlite3_column_double(stmt, 8);
        block.pool_fee_amount = sqlite3_column_int64(stmt, 9);
        block.distributable = sqlite3_column_int64(stmt, 10);
        block.round_shares = sqlite3_column_int64(stmt, 11);
        block.round_difficulty = sqlite3_column_double(stmt, 12);
        block.confirmations = sqlite3_column_int(stmt, 13);
        block.required_confirmations = sqlite3_column_int(stmt, 14);
        block.orphaned = sqlite3_column_int(stmt, 15) != 0;
        block.payouts_calculated = sqlite3_column_int(stmt, 16) != 0;
        block.payouts_sent = sqlite3_column_int(stmt, 17) != 0;
        block.found_at = sqlite3_column_int64(stmt, 18);
        block.confirmed_at = sqlite3_column_int64(stmt, 19);
        blocks.push_back(block);
    }
    sqlite3_finalize(stmt);
    return blocks;
}

std::vector<Share> PoolDB::getSharesInRange(int64_t start_time, int64_t end_time) {
    std::vector<Share> shares;
    const char* sql = "SELECT * FROM shares WHERE submitted_at >= ? AND submitted_at <= ? ORDER BY submitted_at DESC";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return shares;
    }
    sqlite3_bind_int64(stmt, 1, start_time);
    sqlite3_bind_int64(stmt, 2, end_time);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Share share;
        share.share_id = sqlite3_column_int64(stmt, 0);
        share.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        share.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        share.job_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        share.difficulty = sqlite3_column_int(stmt, 4);
        share.difficulty_real = sqlite3_column_double(stmt, 5);
        share.status = static_cast<ShareStatus>(sqlite3_column_int(stmt, 6));
        if (sqlite3_column_text(stmt, 7)) {
            share.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        }
        share.block_height = sqlite3_column_int(stmt, 8);
        share.block_reward = sqlite3_column_int64(stmt, 9);
        share.submitted_at = sqlite3_column_int64(stmt, 10);
        shares.push_back(share);
    }
    sqlite3_finalize(stmt);
    return shares;
}

std::vector<Payout> PoolDB::getPendingPayouts() {
    std::vector<Payout> payouts;
    const char* sql = "SELECT * FROM payouts WHERE status IN (0, 3) ORDER BY calculated_at ASC";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return payouts;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int column_count = sqlite3_column_count(stmt);
        Payout payout;
        payout.payout_id = sqlite3_column_int64(stmt, 0);
        payout.block_id = sqlite3_column_int64(stmt, 1);
        payout.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        payout.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        payout.amount = sqlite3_column_int64(stmt, 4);
        payout.share_percent = sqlite3_column_double(stmt, 5);
        payout.share_count = sqlite3_column_int64(stmt, 6);
        payout.difficulty_sum = sqlite3_column_double(stmt, 7);
        payout.status = static_cast<PayoutStatus>(sqlite3_column_int(stmt, 8));
        if (sqlite3_column_text(stmt, 9)) {
            payout.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        }
        if (sqlite3_column_text(stmt, 10)) {
            payout.error_message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        }
        payout.calculated_at = sqlite3_column_int64(stmt, 11);
        payout.paid_at = sqlite3_column_int64(stmt, 12);
        payout.retry_count = column_count > 13 ? static_cast<uint32_t>(sqlite3_column_int(stmt, 13)) : 0;
        payout.last_retry_at = column_count > 14 ? sqlite3_column_int64(stmt, 14) : 0;
        payouts.push_back(payout);
    }
    sqlite3_finalize(stmt);
    return payouts;
}

std::vector<Payout> PoolDB::getWorkerPayouts(const std::string& worker_id,
                                             uint32_t limit) {
    std::vector<Payout> payouts;
    const char* sql = R"(
        SELECT * FROM payouts
        WHERE worker_id = ?
        ORDER BY calculated_at DESC
        LIMIT ?;
    )";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return payouts;
    }
    sqlite3_bind_text(stmt, 1, worker_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(limit));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int column_count = sqlite3_column_count(stmt);
        Payout payout;
        payout.payout_id = sqlite3_column_int64(stmt, 0);
        payout.block_id = sqlite3_column_int64(stmt, 1);
        payout.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        payout.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        payout.amount = sqlite3_column_int64(stmt, 4);
        payout.share_percent = sqlite3_column_double(stmt, 5);
        payout.share_count = sqlite3_column_int64(stmt, 6);
        payout.difficulty_sum = sqlite3_column_double(stmt, 7);
        payout.status = static_cast<PayoutStatus>(sqlite3_column_int(stmt, 8));
        if (sqlite3_column_text(stmt, 9)) {
            payout.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        }
        if (sqlite3_column_text(stmt, 10)) {
            payout.error_message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        }
        payout.calculated_at = sqlite3_column_int64(stmt, 11);
        payout.paid_at = sqlite3_column_int64(stmt, 12);
        payout.retry_count = column_count > 13 ? static_cast<uint32_t>(sqlite3_column_int(stmt, 13)) : 0;
        payout.last_retry_at = column_count > 14 ? sqlite3_column_int64(stmt, 14) : 0;
        payouts.push_back(payout);
    }
    sqlite3_finalize(stmt);
    return payouts;
}

std::vector<Payout> PoolDB::getPayoutsForBlock(uint64_t block_id) {
    std::vector<Payout> payouts;
    const char* sql = R"(
        SELECT * FROM payouts
        WHERE block_id = ?
        ORDER BY amount DESC, payout_id ASC;
    )";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return payouts;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(block_id));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int column_count = sqlite3_column_count(stmt);
        Payout payout;
        payout.payout_id = sqlite3_column_int64(stmt, 0);
        payout.block_id = sqlite3_column_int64(stmt, 1);
        payout.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        payout.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        payout.amount = sqlite3_column_int64(stmt, 4);
        payout.share_percent = sqlite3_column_double(stmt, 5);
        payout.share_count = sqlite3_column_int64(stmt, 6);
        payout.difficulty_sum = sqlite3_column_double(stmt, 7);
        payout.status = static_cast<PayoutStatus>(sqlite3_column_int(stmt, 8));
        if (sqlite3_column_text(stmt, 9)) {
            payout.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        }
        if (sqlite3_column_text(stmt, 10)) {
            payout.error_message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        }
        payout.calculated_at = sqlite3_column_int64(stmt, 11);
        payout.paid_at = sqlite3_column_int64(stmt, 12);
        payout.retry_count = column_count > 13 ? static_cast<uint32_t>(sqlite3_column_int(stmt, 13)) : 0;
        payout.last_retry_at = column_count > 14 ? sqlite3_column_int64(stmt, 14) : 0;
        payouts.push_back(payout);
    }
    sqlite3_finalize(stmt);
    return payouts;
}

std::vector<Payout> PoolDB::getPayoutsReadyToSend() {
    std::vector<Payout> payouts;
    // Get payouts where block is confirmed (status = CONFIRMED = 1)
    const char* sql = "SELECT * FROM payouts WHERE status = 1 ORDER BY calculated_at ASC";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return payouts;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int column_count = sqlite3_column_count(stmt);
        Payout payout;
        payout.payout_id = sqlite3_column_int64(stmt, 0);
        payout.block_id = sqlite3_column_int64(stmt, 1);
        payout.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        payout.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        payout.amount = sqlite3_column_int64(stmt, 4);
        payout.share_percent = sqlite3_column_double(stmt, 5);
        payout.share_count = sqlite3_column_int64(stmt, 6);
        payout.difficulty_sum = sqlite3_column_double(stmt, 7);
        payout.status = static_cast<PayoutStatus>(sqlite3_column_int(stmt, 8));
        if (sqlite3_column_text(stmt, 9)) {
            payout.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        }
        if (sqlite3_column_text(stmt, 10)) {
            payout.error_message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        }
        payout.calculated_at = sqlite3_column_int64(stmt, 11);
        payout.paid_at = sqlite3_column_int64(stmt, 12);
        payout.retry_count = column_count > 13 ? static_cast<uint32_t>(sqlite3_column_int(stmt, 13)) : 0;
        payout.last_retry_at = column_count > 14 ? sqlite3_column_int64(stmt, 14) : 0;
        payouts.push_back(payout);
    }
    sqlite3_finalize(stmt);
    return payouts;
}

bool PoolDB::markBlockOrphaned(uint64_t block_id) {
    const char* sql = "UPDATE blocks SET orphaned = 1 WHERE block_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, block_id);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool PoolDB::updatePayoutStatus(uint64_t payout_id, PayoutStatus status,
                                const std::string& txid, const std::string& error) {
    const char* sql = R"(
        UPDATE payouts SET
            status = ?,
            txid = ?,
            error_message = ?,
            paid_at = ?
        WHERE payout_id = ?
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(status));
    sqlite3_bind_text(stmt, 2, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, status == PayoutStatus::PAID ? std::time(nullptr) : 0);
    sqlite3_bind_int64(stmt, 5, payout_id);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool PoolDB::incrementPayoutRetry(uint64_t payout_id, int64_t retry_time, const std::string& error) {
    const char* sql = R"(
        UPDATE payouts SET
            retry_count = retry_count + 1,
            last_retry_at = ?,
            error_message = CASE
                WHEN ? != '' THEN ?
                ELSE error_message
            END
        WHERE payout_id = ?
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, retry_time);
    sqlite3_bind_text(stmt, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, payout_id);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

std::vector<PoolBlock> PoolDB::getBlocksReadyForPayout() {
    std::vector<PoolBlock> blocks;
    const char* sql = R"(
        SELECT * FROM blocks
        WHERE orphaned = 0
        AND payouts_calculated = 0
        AND confirmations >= required_confirmations
        ORDER BY found_at ASC
    )";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return blocks;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PoolBlock block;
        block.block_id = sqlite3_column_int64(stmt, 0);
        block.block_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        block.height = sqlite3_column_int(stmt, 2);
        block.finder_worker = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        block.finder_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        block.reward = sqlite3_column_int64(stmt, 5);
        block.fees = sqlite3_column_int64(stmt, 6);
        block.total_reward = sqlite3_column_int64(stmt, 7);
        block.pool_fee_percent = sqlite3_column_double(stmt, 8);
        block.pool_fee_amount = sqlite3_column_int64(stmt, 9);
        block.distributable = sqlite3_column_int64(stmt, 10);
        block.round_shares = sqlite3_column_int64(stmt, 11);
        block.round_difficulty = sqlite3_column_double(stmt, 12);
        block.confirmations = sqlite3_column_int(stmt, 13);
        block.required_confirmations = sqlite3_column_int(stmt, 14);
        block.orphaned = sqlite3_column_int(stmt, 15) != 0;
        block.payouts_calculated = sqlite3_column_int(stmt, 16) != 0;
        block.payouts_sent = sqlite3_column_int(stmt, 17) != 0;
        block.found_at = sqlite3_column_int64(stmt, 18);
        block.confirmed_at = sqlite3_column_int64(stmt, 19);
        blocks.push_back(block);
    }
    sqlite3_finalize(stmt);
    return blocks;
}

std::vector<WorkerStats> PoolDB::getWorkersWithPendingBalance(uint64_t min_balance) {
    std::vector<WorkerStats> workers;
    const char* sql = "SELECT * FROM workers WHERE pending_payout >= ? ORDER BY pending_payout DESC";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return workers;
    }
    sqlite3_bind_int64(stmt, 1, min_balance);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WorkerStats w;
        w.worker_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        w.wallet_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        w.shares_valid = sqlite3_column_int64(stmt, 2);
        w.shares_stale = sqlite3_column_int64(stmt, 3);
        w.shares_invalid = sqlite3_column_int64(stmt, 4);
        w.blocks_found = sqlite3_column_int64(stmt, 5);
        w.current_difficulty = sqlite3_column_double(stmt, 6);
        w.total_difficulty = sqlite3_column_double(stmt, 7);
        w.hashrate_1m = sqlite3_column_double(stmt, 8);
        w.hashrate_15m = sqlite3_column_double(stmt, 9);
        w.hashrate_1h = sqlite3_column_double(stmt, 10);
        w.hashrate_24h = sqlite3_column_double(stmt, 11);
        w.total_earned = sqlite3_column_int64(stmt, 12);
        w.pending_payout = sqlite3_column_int64(stmt, 13);
        w.total_paid = sqlite3_column_int64(stmt, 14);
        w.first_seen = sqlite3_column_int64(stmt, 15);
        w.last_seen = sqlite3_column_int64(stmt, 16);
        w.last_share = sqlite3_column_int64(stmt, 17);
        workers.push_back(w);
    }
    sqlite3_finalize(stmt);
    return workers;
}

std::optional<MiningRound> PoolDB::getRound(uint64_t round_id) {
    const char* sql = "SELECT * FROM rounds WHERE round_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, round_id);

    std::optional<MiningRound> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        MiningRound round;
        round.round_id = sqlite3_column_int64(stmt, 0);
        round.block_id = sqlite3_column_int64(stmt, 1);
        round.total_shares = sqlite3_column_int64(stmt, 2);
        round.total_difficulty = sqlite3_column_double(stmt, 3);
        round.started_at = sqlite3_column_int64(stmt, 4);
        round.ended_at = sqlite3_column_int64(stmt, 5);
        result = round;

        // Load worker difficulties from round_shares table
        const char* shares_sql = "SELECT worker_id, difficulty_sum FROM round_shares WHERE round_id = ?";
        sqlite3_stmt* shares_stmt;
        if (sqlite3_prepare_v2(db_, shares_sql, -1, &shares_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(shares_stmt, 1, round_id);
            while (sqlite3_step(shares_stmt) == SQLITE_ROW) {
                std::string worker_id = reinterpret_cast<const char*>(sqlite3_column_text(shares_stmt, 0));
                double diff = sqlite3_column_double(shares_stmt, 1);
                result->worker_difficulty[worker_id] = diff;
            }
            sqlite3_finalize(shares_stmt);
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

} // namespace pool
} // namespace dinero
