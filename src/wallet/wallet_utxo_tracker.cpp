#include "wallet/wallet_utxo_tracker.h"
#include "common/logger.h"
#include <filesystem>
#include <sstream>

namespace dinero {
namespace wallet {

// ═══════════════════════════════════════════════════════════════════════════
// Schema Definition
// ═══════════════════════════════════════════════════════════════════════════

static const char* WALLET_UTXO_SCHEMA = R"(
CREATE TABLE IF NOT EXISTS wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,
    cached_amount INTEGER NOT NULL,
    cached_height INTEGER NOT NULL,
    state INTEGER NOT NULL DEFAULT 0,
    spending_txid TEXT,
    confirmations INTEGER NOT NULL DEFAULT 0,
    is_coinbase INTEGER NOT NULL DEFAULT 0,
    ancestor_count INTEGER NOT NULL DEFAULT 0,
    label TEXT,
    PRIMARY KEY (txid, vout)
);

CREATE INDEX IF NOT EXISTS idx_state ON wallet_utxos(state);
CREATE INDEX IF NOT EXISTS idx_path ON wallet_utxos(derivation_path);
CREATE INDEX IF NOT EXISTS idx_height ON wallet_utxos(cached_height);
)";

// ═══════════════════════════════════════════════════════════════════════════
// WalletUTXOTracker Implementation
// ═══════════════════════════════════════════════════════════════════════════

WalletUTXOTracker::WalletUTXOTracker(const std::string& db_path,
                                     consensus::GlobalUTXOSet* global_utxo_set)
    : db_(nullptr), db_path_(db_path), global_utxo_set_(global_utxo_set),
      stmt_add_utxo_(nullptr), stmt_mark_spent_(nullptr),
      stmt_get_utxo_(nullptr), stmt_get_unspent_(nullptr),
      stmt_owns_utxo_(nullptr) {
}

WalletUTXOTracker::~WalletUTXOTracker() {
    close();
}

bool WalletUTXOTracker::initialize() {
    if (db_ != nullptr) {
        g_logger.error("[WalletUTXOTracker] Already initialized");
        return false;
    }

    // Create parent directory if needed
    std::filesystem::path db_file(db_path_);
    std::filesystem::create_directories(db_file.parent_path());

    // Open SQLite database
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to open database: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    // Enable WAL mode for better concurrency
    char* err_msg = nullptr;
    rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to enable WAL: " +
                      std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }

    // Create schema
    if (!createSchema()) {
        return false;
    }

    // Prepare statements
    if (!prepareStatements()) {
        return false;
    }

    g_logger.info("[WalletUTXOTracker] Initialized at: " + db_path_);
    return true;
}

void WalletUTXOTracker::close() {
    finalizeStatements();

    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
        g_logger.info("[WalletUTXOTracker] Closed");
    }
}

bool WalletUTXOTracker::createSchema() {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, WALLET_UTXO_SCHEMA, nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to create schema: " +
                      std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool WalletUTXOTracker::prepareStatements() {
    // Add UTXO statement
    const char* sql_add = R"(
        INSERT OR REPLACE INTO wallet_utxos
        (txid, vout, derivation_path, cached_amount, cached_height, state, spending_txid, confirmations, is_coinbase, ancestor_count, label)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    if (sqlite3_prepare_v2(db_, sql_add, -1, &stmt_add_utxo_, nullptr) != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to prepare add statement");
        return false;
    }

    // Mark spent statement (now updates state to SPENT_LOCAL)
    const char* sql_mark_spent = "UPDATE wallet_utxos SET state = ? WHERE txid = ? AND vout = ?";

    if (sqlite3_prepare_v2(db_, sql_mark_spent, -1, &stmt_mark_spent_, nullptr) != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to prepare mark spent statement");
        return false;
    }

    // Get UTXO statement
    const char* sql_get = "SELECT * FROM wallet_utxos WHERE txid = ? AND vout = ?";

    if (sqlite3_prepare_v2(db_, sql_get, -1, &stmt_get_utxo_, nullptr) != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to prepare get statement");
        return false;
    }

    // Get unspent statement (state = CONFIRMED or UNCONFIRMED)
    const char* sql_unspent = "SELECT * FROM wallet_utxos WHERE state IN (0, 1)";

    if (sqlite3_prepare_v2(db_, sql_unspent, -1, &stmt_get_unspent_, nullptr) != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to prepare unspent statement");
        return false;
    }

    // Owns UTXO statement
    const char* sql_owns = "SELECT 1 FROM wallet_utxos WHERE txid = ? AND vout = ? LIMIT 1";

    if (sqlite3_prepare_v2(db_, sql_owns, -1, &stmt_owns_utxo_, nullptr) != SQLITE_OK) {
        g_logger.error("[WalletUTXOTracker] Failed to prepare owns statement");
        return false;
    }

    return true;
}

void WalletUTXOTracker::finalizeStatements() {
    if (stmt_add_utxo_) sqlite3_finalize(stmt_add_utxo_);
    if (stmt_mark_spent_) sqlite3_finalize(stmt_mark_spent_);
    if (stmt_get_utxo_) sqlite3_finalize(stmt_get_utxo_);
    if (stmt_get_unspent_) sqlite3_finalize(stmt_get_unspent_);
    if (stmt_owns_utxo_) sqlite3_finalize(stmt_owns_utxo_);

    stmt_add_utxo_ = nullptr;
    stmt_mark_spent_ = nullptr;
    stmt_get_utxo_ = nullptr;
    stmt_get_unspent_ = nullptr;
    stmt_owns_utxo_ = nullptr;
}

bool WalletUTXOTracker::addOwnedUTXO(const WalletUTXO& utxo) {
    if (!isOpen()) {
        g_logger.error("[WalletUTXOTracker] Database not open");
        return false;
    }

    sqlite3_reset(stmt_add_utxo_);
    sqlite3_bind_text(stmt_add_utxo_, 1, utxo.txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_add_utxo_, 2, utxo.vout);
    sqlite3_bind_text(stmt_add_utxo_, 3, utxo.derivation_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_add_utxo_, 4, utxo.cached_amount);
    sqlite3_bind_int(stmt_add_utxo_, 5, utxo.cached_height);
    sqlite3_bind_int(stmt_add_utxo_, 6, static_cast<int>(utxo.state));

    if (utxo.spending_txid) {
        sqlite3_bind_text(stmt_add_utxo_, 7, utxo.spending_txid->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 7);
    }

    sqlite3_bind_int(stmt_add_utxo_, 8, utxo.confirmations);
    sqlite3_bind_int(stmt_add_utxo_, 9, utxo.is_coinbase ? 1 : 0);
    sqlite3_bind_int(stmt_add_utxo_, 10, utxo.ancestor_count);
    sqlite3_bind_text(stmt_add_utxo_, 11, utxo.label.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt_add_utxo_);

    if (rc != SQLITE_DONE) {
        g_logger.error("[WalletUTXOTracker] Failed to add UTXO: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    return true;
}

bool WalletUTXOTracker::markSpent(const std::string& txid, uint32_t vout) {
    if (!isOpen()) {
        return false;
    }

    sqlite3_reset(stmt_mark_spent_);
    sqlite3_bind_int(stmt_mark_spent_, 1, static_cast<int>(WalletUTXOState::SPENT_LOCAL));
    sqlite3_bind_text(stmt_mark_spent_, 2, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_mark_spent_, 3, vout);

    int rc = sqlite3_step(stmt_mark_spent_);

    return rc == SQLITE_DONE;
}

bool WalletUTXOTracker::ownsUTXO(const std::string& txid, uint32_t vout) const {
    if (!isOpen()) {
        return false;
    }

    sqlite3_reset(stmt_owns_utxo_);
    sqlite3_bind_text(stmt_owns_utxo_, 1, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_owns_utxo_, 2, vout);

    int rc = sqlite3_step(stmt_owns_utxo_);

    return rc == SQLITE_ROW;
}

std::optional<WalletUTXO> WalletUTXOTracker::getWalletUTXO(const std::string& txid, uint32_t vout) const {
    if (!isOpen()) {
        return std::nullopt;
    }

    sqlite3_reset(stmt_get_utxo_);
    sqlite3_bind_text(stmt_get_utxo_, 1, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_get_utxo_, 2, vout);

    if (sqlite3_step(stmt_get_utxo_) != SQLITE_ROW) {
        return std::nullopt;
    }

    WalletUTXO utxo;
    utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 0));
    utxo.vout = sqlite3_column_int(stmt_get_utxo_, 1);
    utxo.derivation_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 2));
    utxo.cached_amount = sqlite3_column_int64(stmt_get_utxo_, 3);
    utxo.cached_height = sqlite3_column_int(stmt_get_utxo_, 4);
    utxo.state = static_cast<WalletUTXOState>(sqlite3_column_int(stmt_get_utxo_, 5));

    const char* spending_txid_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 6));
    if (spending_txid_ptr) {
        utxo.spending_txid = spending_txid_ptr;
    }

    utxo.confirmations = sqlite3_column_int(stmt_get_utxo_, 7);
    utxo.is_coinbase = sqlite3_column_int(stmt_get_utxo_, 8) != 0;
    utxo.ancestor_count = sqlite3_column_int(stmt_get_utxo_, 9);

    const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 10));
    if (label) {
        utxo.label = label;
    }

    return utxo;
}

std::vector<WalletUTXO> WalletUTXOTracker::getUnspentUTXOs(bool include_locked) const {
    std::vector<WalletUTXO> utxos;

    if (!isOpen()) {
        return utxos;
    }

    sqlite3_reset(stmt_get_unspent_);

    while (sqlite3_step(stmt_get_unspent_) == SQLITE_ROW) {
        WalletUTXO utxo;
        utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_unspent_, 0));
        utxo.vout = sqlite3_column_int(stmt_get_unspent_, 1);
        utxo.derivation_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_unspent_, 2));
        utxo.cached_amount = sqlite3_column_int64(stmt_get_unspent_, 3);
        utxo.cached_height = sqlite3_column_int(stmt_get_unspent_, 4);
        utxo.state = static_cast<WalletUTXOState>(sqlite3_column_int(stmt_get_unspent_, 5));

        const char* spending_txid_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_unspent_, 6));
        if (spending_txid_ptr) {
            utxo.spending_txid = spending_txid_ptr;
        }

        utxo.confirmations = sqlite3_column_int(stmt_get_unspent_, 7);
        utxo.is_coinbase = sqlite3_column_int(stmt_get_unspent_, 8) != 0;
        utxo.ancestor_count = sqlite3_column_int(stmt_get_unspent_, 9);

        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_unspent_, 10));
        if (label) {
            utxo.label = label;
        }

        // Filter locked if requested (use helper method)
        if (!include_locked && utxo.isLocked()) {
            continue;
        }

        utxos.push_back(utxo);
    }

    return utxos;
}

WalletBalance WalletUTXOTracker::getBalance(uint32_t current_height) const {
    WalletBalance balance;

    std::vector<WalletUTXO> utxos = getUnspentUTXOs(true);  // Include locked for total

    for (const auto& utxo : utxos) {
        // TODO(Milestone 12.X): Integrate with GlobalUTXOSet when implemented
        // Verify UTXO still exists in global set
        // if (global_utxo_set_ && !global_utxo_set_->hasUTXO(utxo.txid, utxo.vout)) {
        //     continue;  // UTXO was spent in global set
        // }

        // Check if mature
        if (isMature(utxo, current_height)) {
            balance.confirmed += utxo.cached_amount;

            if (utxo.isLocked()) {
                balance.locked += utxo.cached_amount;
            }
        } else {
            balance.immature += utxo.cached_amount;
        }
    }

    balance.total = balance.confirmed + balance.immature;

    return balance;
}

std::vector<WalletUTXO> WalletUTXOTracker::getSpendableUTXOs(uint32_t current_height,
                                                              uint32_t min_confirmations) const {
    std::vector<WalletUTXO> spendable;

    std::vector<WalletUTXO> utxos = getUnspentUTXOs(false);  // Exclude locked

    for (const auto& utxo : utxos) {
        // TODO(Milestone 12.X): Integrate with GlobalUTXOSet when implemented
        // Verify exists in global set
        // if (global_utxo_set_ && !global_utxo_set_->hasUTXO(utxo.txid, utxo.vout)) {
        //     continue;
        // }

        // Check maturity
        if (!isMature(utxo, current_height)) {
            continue;
        }

        // Check confirmations
        uint32_t confirmations = current_height - utxo.cached_height + 1;
        if (confirmations < min_confirmations) {
            continue;
        }

        spendable.push_back(utxo);
    }

    return spendable;
}

bool WalletUTXOTracker::isMature(const WalletUTXO& utxo, uint32_t current_height) const {
    // Check coinbase maturity using cached flag (Milestone 12.2)
    if (utxo.is_coinbase) {
        // Coinbase needs 100 confirmations
        uint32_t confirmations = current_height - utxo.cached_height + 1;
        return confirmations >= 100;
    }

    // Regular UTXO is always mature
    return true;
}

bool WalletUTXOTracker::lockUTXO(const std::string& txid, uint32_t vout) {
    if (!isOpen()) {
        return false;
    }

    const char* sql = "UPDATE wallet_utxos SET state = ? WHERE txid = ? AND vout = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(WalletUTXOState::LOCKED));
    sqlite3_bind_text(stmt, 2, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, vout);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool WalletUTXOTracker::unlockUTXO(const std::string& txid, uint32_t vout) {
    if (!isOpen()) {
        return false;
    }

    // Get current UTXO to determine correct state to transition to
    auto utxo = getWalletUTXO(txid, vout);
    if (!utxo) {
        return false;
    }

    // When unlocking, transition to CONFIRMED (most common case)
    // Wallet layer should update this based on confirmations
    const char* sql = "UPDATE wallet_utxos SET state = ? WHERE txid = ? AND vout = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(WalletUTXOState::CONFIRMED));
    sqlite3_bind_text(stmt, 2, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, vout);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool WalletUTXOTracker::isLocked(const std::string& txid, uint32_t vout) const {
    auto utxo = getWalletUTXO(txid, vout);
    return utxo && utxo->isLocked();
}

bool WalletUTXOTracker::setLabel(const std::string& txid, uint32_t vout, const std::string& label) {
    if (!isOpen()) {
        return false;
    }

    const char* sql = "UPDATE wallet_utxos SET label = ? WHERE txid = ? AND vout = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, txid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, vout);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

uint64_t WalletUTXOTracker::verifyAgainstGlobalSet() const {
    // TODO(Milestone 12.X): Implement when GlobalUTXOSet is available
    if (!isOpen()) {
        return 0;
    }

    // Placeholder: Will verify against global UTXO set when available
    return 0;

    // uint64_t missing_count = 0;
    // std::vector<WalletUTXO> utxos = getUnspentUTXOs(true);
    //
    // for (const auto& utxo : utxos) {
    //     if (!global_utxo_set_->hasUTXO(utxo.txid, utxo.vout)) {
    //         missing_count++;
    //         g_logger.warning("[WalletUTXOTracker] UTXO no longer in global set: " +
    //                        utxo.txid + ":" + std::to_string(utxo.vout));
    //     }
    // }
    //
    // return missing_count;
}

uint64_t WalletUTXOTracker::rescan() {
    // TODO(Milestone 12.X): Implement when GlobalUTXOSet is available
    if (!isOpen()) {
        return 0;
    }

    // Placeholder: Will rescan against global UTXO set when available
    return 0;

    // uint64_t removed_count = 0;
    // std::vector<WalletUTXO> utxos = getUnspentUTXOs(true);
    //
    // for (const auto& utxo : utxos) {
    //     if (!global_utxo_set_->hasUTXO(utxo.txid, utxo.vout)) {
    //         // Mark as spent (since it no longer exists)
    //         if (markSpent(utxo.txid, utxo.vout)) {
    //             removed_count++;
    //             g_logger.info("[WalletUTXOTracker] Marked spent (no longer in global set): " +
    //                         utxo.txid + ":" + std::to_string(utxo.vout));
    //         }
    //     }
    // }
    //
    // return removed_count;
}

uint64_t WalletUTXOTracker::getUTXOCount() const {
    if (!isOpen()) {
        return 0;
    }

    const char* sql = "SELECT COUNT(*) FROM wallet_utxos";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

uint64_t WalletUTXOTracker::getUnspentCount() const {
    if (!isOpen()) {
        return 0;
    }

    const char* sql = "SELECT COUNT(*) FROM wallet_utxos WHERE is_spent = 0";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

} // namespace wallet
} // namespace dinero
