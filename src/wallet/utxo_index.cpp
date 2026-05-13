#include "wallet/utxo_index.h"
#include "sqlite_open.h"
#include "wallet/hd_wallet.h"
#include "address/addr_codec.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <cassert>

namespace dinero {

// =============================================================================
// WALLET INVARIANT: Derivation Path Validation
// =============================================================================
// Rule: Every wallet UTXO MUST have a valid derivation path.
// A valid path starts with "m/" and follows BIP32 format.
// Empty or invalid paths indicate ownership tracking failure.
// =============================================================================

/**
 * @brief Validate that a derivation path is valid for wallet UTXOs
 * @param path The derivation path string (e.g., "m/86'/1448'/0'/0/12")
 * @return true if path is valid BIP32 format, false otherwise
 *
 * Valid paths:
 *   - "m/86'/1448'/0'/0/0"  (BIP86 Taproot - PRIMARY)
 *   - "m/84'/1448'/0'/0/0"  (BIP84 P2WPKH - LEGACY)
 *   - "m/77'/1448'/..."     (Shielded key hierarchy)
 *
 * Invalid paths:
 *   - ""                    (empty - ownership unknown)
 *   - "unknown"             (placeholder - not derived)
 *   - anything not starting with "m/"
 */
static bool IsValidDerivationPath(const std::string& path) {
    // Must not be empty
    if (path.empty()) {
        return false;
    }

    // BIP-style derivation: "m/86'/...", "m/84'/...", etc.
    if (path.size() >= 4 && path[0] == 'm' && path[1] == '/') {
        return true;
    }

    // Descriptor-imported Taproot keys: "tr(...)"
    // These are valid ownership proofs — the wallet holds the private key.
    if (path.size() >= 4 && path.substr(0, 3) == "tr(") {
        return true;
    }

    return false;
}

/**
 * @brief Check if a path is explicitly marked as "external" (non-wallet)
 * @param path The path string
 * @return true if this is a known external/system path
 *
 * Some UTXOs may legitimately have non-wallet paths:
 *   - "genesis" - Genesis outputs
 *   - "coinbase" - Mining rewards (before wallet claims)
 */
static bool IsExternalPath(const std::string& path) {
    return path == "genesis" || path == "coinbase" || path == "system";
}

UTXOIndex::UTXOIndex(const std::string& db_path)
    : db_(nullptr), db_path_(db_path), stmt_add_utxo_(nullptr),
      stmt_spend_utxo_(nullptr), stmt_get_unspent_(nullptr),
      stmt_get_balance_(nullptr), stmt_is_spent_(nullptr), stmt_get_utxo_(nullptr),
      stmt_get_position_(nullptr) {  // Phase 11a: Utreexo position tracking
}

UTXOIndex::~UTXOIndex() {
    FinalizeStatements();
    if (db_) {
        sqlite3_close(db_);
    }
}

bool UTXOIndex::Initialize() {
    // M.5.2: Guard against re-initialization (lifecycle safety)
    // If already initialized, clean up first to prevent memory leaks
    if (db_ != nullptr) {
        std::cerr << "WARNING: UTXOIndex::Initialize() called on already-initialized instance" << std::endl;
        std::cerr << "         Cleaning up previous state before re-initializing..." << std::endl;
        FinalizeStatements();
        sqlite3_close(db_);
        db_ = nullptr;
    }

    // CRITICAL FIX: Use unified SQLite opener with consistent PRAGMAs
    auto opened = open_sqlite(db_path_);
    if (opened.rc != SQLITE_OK) {
        std::cerr << "ERROR: Failed to open UTXO database: " << opened.errmsg << std::endl;
        return false;
    }
    db_ = opened.db;

    if (!CreateTables()) {
        std::cerr << "ERROR: Failed to create UTXO tables" << std::endl;
        return false;
    }

    if (!PrepareStatements()) {
        std::cerr << "ERROR: Failed to prepare UTXO statements" << std::endl;
        return false;
    }

    std::cout << "INFO: UTXO index initialized successfully" << std::endl;
    return true;
}

bool UTXOIndex::CreateTables() {
    // M.5.2: Enhanced diagnostics for schema creation
    std::cout << "[UTXOIndex] CreateTables: db_ = " << (void*)db_ << std::endl;

    if (db_ == nullptr) {
        std::cerr << "FATAL: CreateTables() called with null db_ pointer!" << std::endl;
        return false;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // SCHEMA MIGRATION: Enforce path NOT NULL constraint
    // ═══════════════════════════════════════════════════════════════════════════
    // Schema version 0: Legacy - path allowed NULL (BUG)
    // Schema version 1: path NOT NULL CHECK(length(path) > 0)
    //
    // Migration strategy:
    //   1. Check current schema version
    //   2. If version 0, verify no invalid rows exist (fail if any)
    //   3. Migrate to version 1
    // ═══════════════════════════════════════════════════════════════════════════
    constexpr int CURRENT_SCHEMA_VERSION = 1;

    // Get current schema version
    int schema_version = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                schema_version = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    std::cout << "[UTXOIndex] Current schema version: " << schema_version << std::endl;

    // Check for invalid paths in legacy databases before migration
    if (schema_version < 1) {
        // Check if table exists and has invalid rows
        sqlite3_stmt* check_stmt = nullptr;
        const char* check_sql = R"(
            SELECT COUNT(*) FROM wallet_utxos
            WHERE path IS NULL OR path = '' OR (length(path) < 2)
        )";

        // Table might not exist yet - that's fine
        if (sqlite3_prepare_v2(db_, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(check_stmt) == SQLITE_ROW) {
                int invalid_count = sqlite3_column_int(check_stmt, 0);
                if (invalid_count > 0) {
                    std::cerr << "═══════════════════════════════════════════════════════════════════════════" << std::endl;
                    std::cerr << "FATAL: Schema migration blocked - " << invalid_count << " UTXOs with invalid paths" << std::endl;
                    std::cerr << "═══════════════════════════════════════════════════════════════════════════" << std::endl;
                    std::cerr << "WALLET INVARIANT: A UTXO without a derivation path is NOT owned." << std::endl;
                    std::cerr << std::endl;
                    std::cerr << "Your database contains UTXOs with NULL or empty paths. These are" << std::endl;
                    std::cerr << "potentially dangerous: ghost balances, unspendable outputs, or" << std::endl;
                    std::cerr << "wallet corruption from previous bugs." << std::endl;
                    std::cerr << std::endl;
                    std::cerr << "To fix:" << std::endl;
                    std::cerr << "  1. Backup your wallet database" << std::endl;
                    std::cerr << "  2. Run: sqlite3 <wallet.db> 'DELETE FROM wallet_utxos WHERE path IS NULL OR path = \"\"'" << std::endl;
                    std::cerr << "  3. Perform a full rescan to recover valid UTXOs" << std::endl;
                    std::cerr << "═══════════════════════════════════════════════════════════════════════════" << std::endl;
                    sqlite3_finalize(check_stmt);
                    return false;
                }
            }
            sqlite3_finalize(check_stmt);
        }
    }

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS wallet_utxos (
            txid TEXT NOT NULL,
            vout INTEGER NOT NULL,
            value INTEGER NOT NULL,
            spk BLOB NOT NULL,
            -- WALLET INVARIANT: path is REQUIRED for ownership proof
            -- Valid: "m/86'/...", "m/84'/...", "genesis", "coinbase", "system"
            path TEXT NOT NULL CHECK(length(path) >= 1),
            height INTEGER NOT NULL,
            spend_height INTEGER,
            is_coinbase INTEGER NOT NULL DEFAULT 0,
            -- Phase 11a: Utreexo position tracking for proof generation
            utreexo_position INTEGER,
            -- Zero-Knowledge privacy fields (Phase F)
            is_confidential INTEGER DEFAULT 0,
            commitment BLOB,
            range_proof BLOB,
            blinding_factor BLOB,
            nonce BLOB,
            PRIMARY KEY (txid, vout)
        );

        -- Index for fast unspent UTXO queries (getbalance, listunspent)
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_unspent
        ON wallet_utxos(spend_height) WHERE spend_height IS NULL;

        -- Index for querying UTXOs by derivation path
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_path
        ON wallet_utxos(path);

        -- Index for querying UTXOs created at specific height (reorg disconnect)
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_height
        ON wallet_utxos(height);

        -- Index for querying UTXOs spent at specific height (reorg un-spend)
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_spend_height
        ON wallet_utxos(spend_height) WHERE spend_height IS NOT NULL;

        -- Index for fast coinbase maturity queries (is_coinbase + height)
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_coinbase_maturity
        ON wallet_utxos(is_coinbase, height) WHERE spend_height IS NULL;

        -- Index for fast confidential UTXO queries (Phase F)
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_confidential
        ON wallet_utxos(is_confidential) WHERE is_confidential = 1 AND spend_height IS NULL;

        -- Phase 11a: Index for fast Utreexo position lookups (proof generation)
        CREATE INDEX IF NOT EXISTS idx_wallet_utxos_utreexo_position
        ON wallet_utxos(txid, vout, utreexo_position) WHERE utreexo_position IS NOT NULL;

        -- Metadata table for AssumeUTXO state persistence (Crash Safety - CRITICAL-003 fix)
        -- Stores key-value pairs for consensus-critical state that must persist across restarts
        CREATE TABLE IF NOT EXISTS utxo_metadata (
            key TEXT PRIMARY KEY NOT NULL,
            value TEXT NOT NULL
        );
    )";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);

    std::cout << "[UTXOIndex] sqlite3_exec returned: " << rc
              << " (" << sqlite3_errstr(rc) << ")" << std::endl;

    if (rc != SQLITE_OK) {
        std::cerr << "ERROR: SQL error creating tables: " << (err_msg ? err_msg : "null") << std::endl;
        std::cerr << "       Extended error code: " << sqlite3_extended_errcode(db_) << std::endl;
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    // M.5.2: Verify tables were actually created
    const char* verify_sql = "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, verify_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        std::cout << "[UTXOIndex] Tables created:" << std::endl;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* table_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::cout << "  - " << table_name << std::endl;
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "WARNING: Could not verify table creation: " << sqlite3_errmsg(db_) << std::endl;
    }

    // Update schema version to current
    if (schema_version < CURRENT_SCHEMA_VERSION) {
        std::string version_sql = "PRAGMA user_version = " + std::to_string(CURRENT_SCHEMA_VERSION);
        if (sqlite3_exec(db_, version_sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK) {
            std::cout << "[UTXOIndex] Schema upgraded to version " << CURRENT_SCHEMA_VERSION << std::endl;
        }
    }

    return true;
}

bool UTXOIndex::PrepareStatements() {
    // Add UTXO statement (with confidential fields + Phase 11a: utreexo_position)
    //
    // UPSERT semantics for CT fields (blinding_factor, value):
    //   On conflict (re-scan of existing UTXO), preserve the existing
    //   blinding_factor and value when the incoming blinding_factor is NULL
    //   (CT rangeproof rewind failed — wallet was locked at scan time).
    //   This prevents losing recovered CT data across daemon restarts.
    //   All other fields are updated normally (on-chain data can't change).
    const char* add_sql = R"(
        INSERT INTO wallet_utxos
        (txid, vout, value, spk, path, height, spend_height, is_coinbase,
         utreexo_position,
         is_confidential, commitment, range_proof, blinding_factor, nonce)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(txid, vout) DO UPDATE SET
          value            = CASE WHEN excluded.blinding_factor IS NOT NULL
                                  THEN excluded.value
                                  ELSE wallet_utxos.value END,
          spk              = excluded.spk,
          path             = excluded.path,
          height           = excluded.height,
          spend_height     = excluded.spend_height,
          is_coinbase      = excluded.is_coinbase,
          utreexo_position = excluded.utreexo_position,
          is_confidential  = excluded.is_confidential,
          commitment       = excluded.commitment,
          range_proof      = excluded.range_proof,
          blinding_factor  = COALESCE(excluded.blinding_factor, wallet_utxos.blinding_factor),
          nonce            = COALESCE(excluded.nonce, wallet_utxos.nonce)
    )";

    if (sqlite3_prepare_v2(db_, add_sql, -1, &stmt_add_utxo_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare add UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Spend UTXO statement
    const char* spend_sql = R"(
        UPDATE wallet_utxos SET spend_height = ? 
        WHERE txid = ? AND vout = ?
    )";
    
    if (sqlite3_prepare_v2(db_, spend_sql, -1, &stmt_spend_utxo_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare spend UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Get unspent UTXOs statement
    const char* unspent_sql = R"(
        SELECT txid, vout, value, spk, path, height 
        FROM wallet_utxos 
        WHERE spend_height IS NULL 
        ORDER BY value DESC
    )";
    
    if (sqlite3_prepare_v2(db_, unspent_sql, -1, &stmt_get_unspent_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get unspent statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Get balance statement
    const char* balance_sql = R"(
        SELECT COALESCE(SUM(value), 0) 
        FROM wallet_utxos 
        WHERE spend_height IS NULL
    )";
    
    if (sqlite3_prepare_v2(db_, balance_sql, -1, &stmt_get_balance_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get balance statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Check if UTXO is spent statement
    const char* is_spent_sql = R"(
        SELECT spend_height FROM wallet_utxos 
        WHERE txid = ? AND vout = ?
    )";
    
    if (sqlite3_prepare_v2(db_, is_spent_sql, -1, &stmt_is_spent_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare is spent statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // Get specific UTXO statement
    const char* get_utxo_sql = R"(
        SELECT txid, vout, value, spk, path, height, spend_height, is_coinbase, is_confidential,
               commitment, range_proof, blinding_factor, nonce
        FROM wallet_utxos
        WHERE txid = ? AND vout = ?
    )";

    if (sqlite3_prepare_v2(db_, get_utxo_sql, -1, &stmt_get_utxo_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Phase 11a: Get Utreexo position statement
    const char* get_position_sql = R"(
        SELECT utreexo_position
        FROM wallet_utxos
        WHERE txid = ? AND vout = ?
    )";

    if (sqlite3_prepare_v2(db_, get_position_sql, -1, &stmt_get_position_, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get position statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    return true;
}

void UTXOIndex::FinalizeStatements() {
    if (stmt_add_utxo_) { sqlite3_finalize(stmt_add_utxo_); stmt_add_utxo_ = nullptr; }
    if (stmt_spend_utxo_) { sqlite3_finalize(stmt_spend_utxo_); stmt_spend_utxo_ = nullptr; }
    if (stmt_get_unspent_) { sqlite3_finalize(stmt_get_unspent_); stmt_get_unspent_ = nullptr; }
    if (stmt_get_balance_) { sqlite3_finalize(stmt_get_balance_); stmt_get_balance_ = nullptr; }
    if (stmt_is_spent_) { sqlite3_finalize(stmt_is_spent_); stmt_is_spent_ = nullptr; }
    if (stmt_get_utxo_) { sqlite3_finalize(stmt_get_utxo_); stmt_get_utxo_ = nullptr; }
    if (stmt_get_position_) { sqlite3_finalize(stmt_get_position_); stmt_get_position_ = nullptr; }  // Phase 11a
}

bool UTXOIndex::AddUTXO(const WalletUTXO& utxo) {
    // ═══════════════════════════════════════════════════════════════════════════
    // WALLET INVARIANT: Every UTXO must have a valid derivation path
    // ═══════════════════════════════════════════════════════════════════════════
    // This ensures the wallet never credits "ghost UTXOs" without ownership proof.
    // Valid paths: "m/86'/..." (PRIMARY), "m/84'/..." (LEGACY), "m/77'/..." (CT)
    // External paths: "genesis", "coinbase", "system" (for system UTXOs)
    // ═══════════════════════════════════════════════════════════════════════════
    if (!IsValidDerivationPath(utxo.path) && !IsExternalPath(utxo.path)) {
        std::cerr << "ERROR [AddUTXO] INVARIANT VIOLATION: Invalid derivation path" << std::endl;
        std::cerr << "  txid: " << utxo.txid.AsUint256().GetHex() << std::endl;
        std::cerr << "  vout: " << utxo.vout << std::endl;
        std::cerr << "  path: \"" << utxo.path << "\"" << std::endl;
        std::cerr << "  This UTXO cannot be credited without ownership proof!" << std::endl;

        // In debug builds, crash immediately to catch the bug
        assert(false && "WALLET INVARIANT: AddUTXO called with invalid derivation path");

        // In release builds, refuse to add the UTXO (safe failure mode)
        return false;
    }

    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_add_utxo_);

    // Bind standard fields (1-8)
    std::string txid_hex = utxo.txid.AsUint256().GetHex();  // Phase M.4: Convert to hex for SQLite storage
    sqlite3_bind_text(stmt_add_utxo_, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_add_utxo_, 2, utxo.vout);
    // Phase M.6.2: SQLite boundary - extract raw value (safe: MAX_SUPPLY < INT64_MAX)
    sqlite3_bind_int64(stmt_add_utxo_, 3, utxo.value.GetInt64());
    sqlite3_bind_blob(stmt_add_utxo_, 4, utxo.spk.data(), utxo.spk.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt_add_utxo_, 5, utxo.path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_add_utxo_, 6, utxo.height);

    if (utxo.spend_height) {
        sqlite3_bind_int(stmt_add_utxo_, 7, *utxo.spend_height);
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 7);
    }

    sqlite3_bind_int(stmt_add_utxo_, 8, utxo.is_coinbase ? 1 : 0);

    // Phase 11a: Bind Utreexo position (9)
    if (utxo.utreexo_position.has_value()) {
        sqlite3_bind_int64(stmt_add_utxo_, 9, static_cast<sqlite3_int64>(utxo.utreexo_position.value()));
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 9);
    }

    // Bind confidential fields (10-14)
    sqlite3_bind_int(stmt_add_utxo_, 10, utxo.is_confidential ? 1 : 0);

    if (utxo.is_confidential && !utxo.commitment.empty()) {
        sqlite3_bind_blob(stmt_add_utxo_, 11, utxo.commitment.data(), utxo.commitment.size(), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 11);
    }

    if (utxo.is_confidential && !utxo.range_proof.empty()) {
        sqlite3_bind_blob(stmt_add_utxo_, 12, utxo.range_proof.data(), utxo.range_proof.size(), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 12);
    }

    if (utxo.is_confidential && !utxo.blinding_factor.empty()) {
        sqlite3_bind_blob(stmt_add_utxo_, 13, utxo.blinding_factor.data(), utxo.blinding_factor.size(), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 13);
    }

    if (utxo.is_confidential && !utxo.nonce.empty()) {
        sqlite3_bind_blob(stmt_add_utxo_, 14, utxo.nonce.data(), utxo.nonce.size(), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt_add_utxo_, 14);
    }

    int rc = sqlite3_step(stmt_add_utxo_);
    if (rc != SQLITE_DONE) {
        std::cerr << "ERROR: Failed to add UTXO: " << sqlite3_errmsg(db_)
                  << " (code: " << rc << ", extended: " << sqlite3_extended_errcode(db_) << ")" << std::endl;
        std::cerr << "       stmt_add_utxo_ pointer: " << (void*)stmt_add_utxo_ << std::endl;
        std::cerr << "       db_ pointer: " << (void*)db_ << std::endl;
        return false;
    }

    return true;
}

bool UTXOIndex::SpendUTXO(const TxId& txid, uint32_t vout, uint32_t height) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_spend_utxo_);

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_int(stmt_spend_utxo_, 1, height);
    sqlite3_bind_text(stmt_spend_utxo_, 2, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_spend_utxo_, 3, vout);
    
    int rc = sqlite3_step(stmt_spend_utxo_);
    if (rc != SQLITE_DONE) {
        std::cerr << "ERROR: Failed to spend UTXO: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    return sqlite3_changes(db_) > 0;
}

bool UTXOIndex::DeleteUTXO(const TxId& txid, uint32_t vout) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Permanently delete UTXO from database (used during reorg to remove outputs from disconnected blocks)
    const char* delete_sql = "DELETE FROM wallet_utxos WHERE txid = ? AND vout = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare delete UTXO statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_text(stmt, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, vout);

    int rc = sqlite3_step(stmt);
    bool success = (rc == SQLITE_DONE);
    int changes = sqlite3_changes(db_);

    sqlite3_finalize(stmt);

    if (!success) {
        std::cerr << "ERROR: Failed to delete UTXO: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    return changes > 0;
}

bool UTXOIndex::IsUTXOSpent(const TxId& txid, uint32_t vout) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_is_spent_);

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_text(stmt_is_spent_, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_is_spent_, 2, vout);
    
    int rc = sqlite3_step(stmt_is_spent_);
    if (rc == SQLITE_ROW) {
        // Check if spend_height is NULL
        return sqlite3_column_type(stmt_is_spent_, 0) != SQLITE_NULL;
    }
    
    return false; // UTXO not found, consider unspent
}

std::vector<WalletUTXO> UTXOIndex::GetUnspentUTXOs() const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    std::vector<WalletUTXO> utxos;
    sqlite3_reset(stmt_get_unspent_);

    while (sqlite3_step(stmt_get_unspent_) == SQLITE_ROW) {
        WalletUTXO utxo;
        // Phase M.0: Convert hex string from database to uint256
        std::string txid_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_unspent_, 0));
        utxo.txid = TxId(uint256::FromHexUnsafe(txid_hex));
        utxo.vout = sqlite3_column_int(stmt_get_unspent_, 1);
        // Phase M.6.2: SQLite boundary - wrap value in AmountUna
        utxo.value = AmountUna::Una(static_cast<uint64_t>(sqlite3_column_int64(stmt_get_unspent_, 2)));

        // Get scriptPubKey blob
        const void* spk_data = sqlite3_column_blob(stmt_get_unspent_, 3);
        int spk_size = sqlite3_column_bytes(stmt_get_unspent_, 3);
        utxo.spk.assign(static_cast<const uint8_t*>(spk_data),
                       static_cast<const uint8_t*>(spk_data) + spk_size);

        utxo.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_unspent_, 4));
        utxo.height = sqlite3_column_int(stmt_get_unspent_, 5);

        // ═══════════════════════════════════════════════════════════════════════════
        // WALLET INVARIANT: Balance must never include pathless UTXOs
        // ═══════════════════════════════════════════════════════════════════════════
        // A UTXO without a derivation path is NOT owned. No exceptions.
        // If we find such a UTXO in the database, it indicates corruption or bug.
        // ═══════════════════════════════════════════════════════════════════════════
        if (!IsValidDerivationPath(utxo.path) && !IsExternalPath(utxo.path)) {
            std::cerr << "ERROR [GetUnspentUTXOs] INVARIANT VIOLATION: Pathless UTXO in database" << std::endl;
            std::cerr << "  txid: " << txid_hex << std::endl;
            std::cerr << "  vout: " << utxo.vout << std::endl;
            std::cerr << "  path: \"" << utxo.path << "\"" << std::endl;
            std::cerr << "  This UTXO should not exist in wallet database!" << std::endl;
            assert(false && "WALLET INVARIANT: Balance includes UTXO without derivation path");
            // In release builds, skip this UTXO (don't include in balance)
            continue;
        }

        utxos.push_back(utxo);
    }

    return utxos;
}

std::optional<WalletUTXO> UTXOIndex::GetUTXO(const TxId& txid, uint32_t vout) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_get_utxo_);

    std::string txid_hex = txid.AsUint256().GetHex();  // Phase M.4.3-B Step 3: Explicit DB boundary
    sqlite3_bind_text(stmt_get_utxo_, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_get_utxo_, 2, vout);

    if (sqlite3_step(stmt_get_utxo_) == SQLITE_ROW) {
        WalletUTXO utxo;
        // Phase M.0: Convert hex string from database to uint256
        std::string txid_from_db = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 0));
        utxo.txid = TxId(uint256::FromHexUnsafe(txid_from_db));
        utxo.vout = sqlite3_column_int(stmt_get_utxo_, 1);
        // Phase M.6.2: SQLite boundary - wrap value in AmountUna
        utxo.value = AmountUna::Una(static_cast<uint64_t>(sqlite3_column_int64(stmt_get_utxo_, 2)));
        
        // Get scriptPubKey blob
        const void* spk_data = sqlite3_column_blob(stmt_get_utxo_, 3);
        int spk_size = sqlite3_column_bytes(stmt_get_utxo_, 3);
        utxo.spk.assign(static_cast<const uint8_t*>(spk_data), 
                       static_cast<const uint8_t*>(spk_data) + spk_size);
        
        utxo.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_utxo_, 4));
        utxo.height = sqlite3_column_int(stmt_get_utxo_, 5);
        
        // Check spend_height
        if (sqlite3_column_type(stmt_get_utxo_, 6) != SQLITE_NULL) {
            utxo.spend_height = sqlite3_column_int(stmt_get_utxo_, 6);
        }

        // Read is_coinbase (column 7)
        utxo.is_coinbase = (sqlite3_column_int(stmt_get_utxo_, 7) != 0);

        // Read is_confidential (column 8)
        utxo.is_confidential = (sqlite3_column_int(stmt_get_utxo_, 8) != 0);

        // Read CT fields (columns 9-12)
        if (sqlite3_column_type(stmt_get_utxo_, 9) != SQLITE_NULL) {
            const void* commit_data = sqlite3_column_blob(stmt_get_utxo_, 9);
            int commit_size = sqlite3_column_bytes(stmt_get_utxo_, 9);
            utxo.commitment.assign(static_cast<const uint8_t*>(commit_data),
                                   static_cast<const uint8_t*>(commit_data) + commit_size);
        }
        if (sqlite3_column_type(stmt_get_utxo_, 10) != SQLITE_NULL) {
            const void* rp_data = sqlite3_column_blob(stmt_get_utxo_, 10);
            int rp_size = sqlite3_column_bytes(stmt_get_utxo_, 10);
            utxo.range_proof.assign(static_cast<const uint8_t*>(rp_data),
                                    static_cast<const uint8_t*>(rp_data) + rp_size);
        }
        if (sqlite3_column_type(stmt_get_utxo_, 11) != SQLITE_NULL) {
            const void* blind_data = sqlite3_column_blob(stmt_get_utxo_, 11);
            int blind_size = sqlite3_column_bytes(stmt_get_utxo_, 11);
            utxo.blinding_factor.assign(static_cast<const uint8_t*>(blind_data),
                                        static_cast<const uint8_t*>(blind_data) + blind_size);
        }
        if (sqlite3_column_type(stmt_get_utxo_, 12) != SQLITE_NULL) {
            const void* nonce_data = sqlite3_column_blob(stmt_get_utxo_, 12);
            int nonce_size = sqlite3_column_bytes(stmt_get_utxo_, 12);
            utxo.nonce.assign(static_cast<const uint8_t*>(nonce_data),
                              static_cast<const uint8_t*>(nonce_data) + nonce_size);
        }

        return utxo;
    }
    
    return std::nullopt;  // UTXO not found
}

bool UTXOIndex::GetUTXO(const TxId& txid, uint32_t vout, WalletUTXO& utxo) const {
    auto result = GetUTXO(txid, vout);
    if (result) {
        utxo = *result;
        return true;
    }
    return false;
}

// Phase M.6.2: Return AmountUna for type safety
AmountUna UTXOIndex::GetBalance() const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_get_balance_);

    if (sqlite3_step(stmt_get_balance_) == SQLITE_ROW) {
        int64_t raw = sqlite3_column_int64(stmt_get_balance_, 0);
        return AmountUna::Una(static_cast<uint64_t>(raw < 0 ? 0 : raw));
    }

    return AmountUna::Zero();
}

// Phase M.6.2: Return AmountUna for type safety
AmountUna UTXOIndex::GetBalanceForPath(const std::string& path_prefix) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    const char* sql = R"(
        SELECT COALESCE(SUM(value), 0)
        FROM wallet_utxos
        WHERE spend_height IS NULL AND path LIKE ?
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AmountUna::Zero();
    }

    std::string pattern = path_prefix + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_STATIC);

    int64_t balance = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    // Phase M.6.2: Wrap in AmountUna for type safety
    return AmountUna::Una(static_cast<uint64_t>(balance < 0 ? 0 : balance));
}

// Phase 44.1: UTXO count for AssumeUTXO verification
Result<uint64_t> UTXOIndex::GetUTXOCount() const {
    std::lock_guard<std::mutex> lock(db_mutex_);

    // M.5.2 FIX: Correct table name (was "utxos", should be "wallet_utxos")
    const char* sql = "SELECT COUNT(*) FROM wallet_utxos WHERE spend_height IS NULL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::string error = std::string("Failed to prepare count query: ") + sqlite3_errmsg(db_);
        return Result<uint64_t>::Err(error);
    }

    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return Result<uint64_t>::Ok(count);
}

BalanceDetail UTXOIndex::GetBalanceWithMaturity(int current_height) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    BalanceDetail result;

    // Query 1: Get confirmed balance (all non-coinbase + mature coinbase)
    // Mature coinbase = current_height - height >= 100
    const char* confirmed_sql = R"(
        SELECT COALESCE(SUM(value), 0)
        FROM wallet_utxos
        WHERE spend_height IS NULL
          AND (is_coinbase = 0 OR (? - height) >= 100)
    )";

    sqlite3_stmt* stmt_confirmed;
    if (sqlite3_prepare_v2(db_, confirmed_sql, -1, &stmt_confirmed, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt_confirmed, 1, current_height);
        if (sqlite3_step(stmt_confirmed) == SQLITE_ROW) {
            // Phase M.6.2: Wrap value in AmountUna
            int64_t raw = sqlite3_column_int64(stmt_confirmed, 0);
            result.confirmed = AmountUna::Una(static_cast<uint64_t>(raw < 0 ? 0 : raw));
        }
        sqlite3_finalize(stmt_confirmed);
    }

    // Query 2: Get immature balance (coinbase with < 100 confirmations)
    const char* immature_sql = R"(
        SELECT COALESCE(SUM(value), 0)
        FROM wallet_utxos
        WHERE spend_height IS NULL
          AND is_coinbase = 1
          AND (? - height) < 100
    )";

    sqlite3_stmt* stmt_immature;
    if (sqlite3_prepare_v2(db_, immature_sql, -1, &stmt_immature, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt_immature, 1, current_height);
        if (sqlite3_step(stmt_immature) == SQLITE_ROW) {
            // Phase M.6.2: Wrap value in AmountUna
            int64_t raw = sqlite3_column_int64(stmt_immature, 0);
            result.immature = AmountUna::Una(static_cast<uint64_t>(raw < 0 ? 0 : raw));
        }
        sqlite3_finalize(stmt_immature);
    }

    // Phase M.6.3: Use checked arithmetic (overflow safe)
    auto total_result = result.confirmed.Add(result.immature);
    result.total = total_result.value_or(AmountUna::Zero());

    // Query 3: Get confidential balance (Phase F - ZK privacy), filtered to
    // watch scripts owned by the currently active wallet.
    const char* confidential_sql = R"(
        SELECT value, spk
        FROM wallet_utxos
        WHERE is_confidential = 1 AND spend_height IS NULL
    )";

    sqlite3_stmt* stmt_confidential;
    if (sqlite3_prepare_v2(db_, confidential_sql, -1, &stmt_confidential, nullptr) == SQLITE_OK) {
        uint64_t confidential_raw = 0;
        while (sqlite3_step(stmt_confidential) == SQLITE_ROW) {
            const void* spk_data = sqlite3_column_blob(stmt_confidential, 1);
            const int spk_size = sqlite3_column_bytes(stmt_confidential, 1);
            if (!spk_data || spk_size <= 0) {
                continue;
            }

            std::vector<uint8_t> spk(static_cast<const uint8_t*>(spk_data),
                                     static_cast<const uint8_t*>(spk_data) + spk_size);
            {
                std::lock_guard<std::mutex> scripts_lock(scripts_mutex_);
                if (watched_scripts_.find(spk) == watched_scripts_.end()) {
                    continue;
                }
            }

            const int64_t raw = sqlite3_column_int64(stmt_confidential, 0);
            if (raw > 0) {
                confidential_raw += static_cast<uint64_t>(raw);
            }
        }
        result.confidential = AmountUna::Una(confidential_raw);
        sqlite3_finalize(stmt_confidential);
    }

    // Phase M.6.3: Use checked arithmetic (overflow safe)
    auto total_with_conf_result = result.total.Add(result.confidential);
    result.total_with_conf = total_with_conf_result.value_or(result.total);

    // Phase M.6.2: Extract values for logging
    std::cout << "INFO: Balance detail at height " << current_height << ": "
              << "confirmed=" << result.confirmed.GetUna() << " sats, "
              << "immature=" << result.immature.GetUna() << " sats, "
              << "total=" << result.total.GetUna() << " sats, "
              << "confidential=" << result.confidential.GetUna() << " sats, "
              << "total_with_conf=" << result.total_with_conf.GetUna() << " sats" << std::endl;

    return result;
}

std::optional<std::string> UTXOIndex::IsOurScript(const std::vector<uint8_t>& scriptPubKey) const {
    std::lock_guard<std::mutex> lock(scripts_mutex_);

    auto it = watched_scripts_.find(scriptPubKey);
    if (it != watched_scripts_.end()) {
        dinero::g_logger.info("[IsOurScript] MATCH FOUND! Path: " + it->second);
        return it->second;
    }

    return std::nullopt;
}

void UTXOIndex::RegisterAddress(const std::vector<uint8_t>& scriptPubKey, const std::string& derivation_path) {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    watched_scripts_[scriptPubKey] = derivation_path;
}

void UTXOIndex::ClearRegisteredAddresses() {
    std::lock_guard<std::mutex> lock(scripts_mutex_);
    watched_scripts_.clear();
}

void UTXOIndex::ProcessBlock(int height, const std::vector<std::string>& block_txs) {
    // ✅ LOCK: Protect entire SQLite transaction (must be atomic)
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    // Begin transaction for atomic block processing
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    
    try {
        for (const auto& tx_data : block_txs) {
            // Parse transaction and process outputs/inputs
            // This would integrate with your transaction parsing logic
            // For now, this is a placeholder
        }
        
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
        std::cout << "INFO: Processed block " << height << " with " << block_txs.size() << " transactions" << std::endl;
    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to process block " << height << ": " << e.what() << std::endl;
        throw;
    }
}

void UTXOIndex::RevertBlock(int height) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Begin transaction for atomic revert
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

    try {
        // 1. Delete UTXOs created at this height
        const char* delete_sql = "DELETE FROM wallet_utxos WHERE height = ?";
        sqlite3_stmt* stmt_del;
        if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt_del, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt_del, 1, height);
            sqlite3_step(stmt_del);
            int deleted = sqlite3_changes(db_);
            sqlite3_finalize(stmt_del);
            if (deleted > 0) {
                std::cout << "INFO: Reverted " << deleted << " UTXOs created at height " << height << std::endl;
            }
        }

        // 2. Un-spend UTXOs that were spent at this height
        const char* unspend_sql = "UPDATE wallet_utxos SET spend_height = NULL WHERE spend_height = ?";
        sqlite3_stmt* stmt_unspend;
        if (sqlite3_prepare_v2(db_, unspend_sql, -1, &stmt_unspend, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt_unspend, 1, height);
            sqlite3_step(stmt_unspend);
            int unspent = sqlite3_changes(db_);
            sqlite3_finalize(stmt_unspend);
            if (unspent > 0) {
                std::cout << "INFO: Un-spent " << unspent << " UTXOs at height " << height << std::endl;
            }
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to revert block " << height << std::endl;
        throw;
    }
}

// Priority 3 FIX: Validate wallet UTXOs against consensus
// Removes phantom UTXOs that exist in wallet but not in consensus
// This should be called after reorg completes to ensure consistency
size_t UTXOIndex::ValidateAgainstConsensus(std::function<bool(const TxId& txid, uint32_t vout)> consensus_has_utxo) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    size_t phantom_count = 0;
    std::vector<std::pair<std::string, uint32_t>> to_delete;

    // Phase 1: Collect all unspent UTXOs from wallet
    const char* query_sql = R"(
        SELECT txid, vout FROM wallet_utxos WHERE spend_height IS NULL
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, query_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare validation query: " << sqlite3_errmsg(db_) << std::endl;
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* txid_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        uint32_t vout = static_cast<uint32_t>(sqlite3_column_int(stmt, 1));

        if (!txid_hex) continue;

        // Convert hex string to TxId
        TxId txid(uint256::FromHexUnsafe(txid_hex));

        // Check if consensus has this UTXO
        if (!consensus_has_utxo(txid, vout)) {
            to_delete.emplace_back(txid_hex, vout);
        }
    }
    sqlite3_finalize(stmt);

    // Phase 2: Delete phantom UTXOs
    if (!to_delete.empty()) {
        sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

        const char* delete_sql = "DELETE FROM wallet_utxos WHERE txid = ? AND vout = ?";
        sqlite3_stmt* stmt_del;
        if (sqlite3_prepare_v2(db_, delete_sql, -1, &stmt_del, nullptr) == SQLITE_OK) {
            for (const auto& [txid_hex, vout] : to_delete) {
                sqlite3_reset(stmt_del);
                sqlite3_bind_text(stmt_del, 1, txid_hex.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(stmt_del, 2, vout);
                sqlite3_step(stmt_del);

                if (sqlite3_changes(db_) > 0) {
                    phantom_count++;
                    std::cerr << "WARNING: Removed phantom UTXO " << txid_hex.substr(0, 16) << "...:" << vout
                              << " (wallet had it, consensus didn't)" << std::endl;
                }
            }
            sqlite3_finalize(stmt_del);
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }

    if (phantom_count > 0) {
        std::cout << "INFO: ValidateAgainstConsensus removed " << phantom_count << " phantom UTXOs" << std::endl;
    }

    return phantom_count;
}

// Phase M.6.2: Output amounts now use AmountUna
void UTXOIndex::ScanBlockIdempotent(int height, const std::string& block_hash,
                                    const std::vector<std::tuple<std::string,
                                                                std::vector<std::pair<std::vector<uint8_t>, AmountUna>>,
                                                                std::vector<std::pair<std::string, uint32_t>>,
                                                                bool>>& transactions) {
    // ✅ LOCK: Protect entire SQLite transaction (must be atomic)
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Begin transaction for atomic block processing
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

    try {
        int utxos_added = 0;
        int utxos_spent = 0;

        // Process each transaction in the block
        for (const auto& [txid, outputs, inputs, is_coinbase] : transactions) {

            // Phase 1: Add new UTXOs from outputs (idempotent with INSERT OR IGNORE)
            for (size_t vout = 0; vout < outputs.size(); ++vout) {
                const auto& [scriptPubKey, value] = outputs[vout];

                // Check if this output belongs to our wallet
                auto opt_path = IsOurScript(scriptPubKey);
                if (opt_path.has_value()) {
                    // Use INSERT OR IGNORE for idempotency - safe to call multiple times
                    const char* insert_sql = R"(
                        INSERT OR IGNORE INTO wallet_utxos
                        (txid, vout, value, spk, path, height, spend_height, is_coinbase)
                        VALUES (?, ?, ?, ?, ?, ?, NULL, ?)
                    )";

                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 2, static_cast<int>(vout));
                        // Phase M.6.2: Extract raw value for SQLite boundary
                        sqlite3_bind_int64(stmt, 3, value.GetInt64());
                        sqlite3_bind_blob(stmt, 4, scriptPubKey.data(), scriptPubKey.size(), SQLITE_STATIC);
                        sqlite3_bind_text(stmt, 5, opt_path.value().c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 6, height);
                        sqlite3_bind_int(stmt, 7, is_coinbase ? 1 : 0);

                        if (sqlite3_step(stmt) == SQLITE_DONE) {
                            if (sqlite3_changes(db_) > 0) {
                                utxos_added++;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                }
            }

            // Phase 2: Mark spent outputs from inputs (idempotent - only updates if spend_height is NULL)
            if (!is_coinbase) {
                for (const auto& [prev_txid, prev_vout] : inputs) {
                    // Only update if the UTXO exists and is currently unspent (spend_height IS NULL)
                    const char* spend_sql = R"(
                        UPDATE wallet_utxos
                        SET spend_height = ?
                        WHERE txid = ? AND vout = ? AND spend_height IS NULL
                    )";

                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db_, spend_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int(stmt, 1, height);
                        sqlite3_bind_text(stmt, 2, prev_txid.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, 3, prev_vout);

                        if (sqlite3_step(stmt) == SQLITE_DONE) {
                            if (sqlite3_changes(db_) > 0) {
                                utxos_spent++;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                }
            }
        }

        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

        if (utxos_added > 0 || utxos_spent > 0) {
            std::cout << "INFO: Scanned block " << height << " (" << block_hash.substr(0, 16) << "...): "
                      << "+" << utxos_added << " UTXOs, -" << utxos_spent << " spent" << std::endl;
        }

    } catch (const std::exception& e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to scan block " << height << ": " << e.what() << std::endl;
        throw;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        std::cerr << "ERROR: Failed to scan block " << height << " (unknown error)" << std::endl;
        throw;
    }
}

// TransactionProcessor implementation
// Phase M.6.2: Output amounts now use AmountUna
void TransactionProcessor::ProcessTransaction(UTXOIndex& index, const std::string& txid,
                                            const std::vector<std::pair<std::vector<uint8_t>, AmountUna>>& outputs,
                                            const std::vector<std::pair<std::string, uint32_t>>& inputs,
                                            int height) {
    // Phase M.0: Convert txid string to uint256 for UTXO operations
    uint256 txid_uint256 = uint256::FromHexUnsafe(txid);

    // Process outputs (potential new UTXOs)
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& [scriptPubKey, value] = outputs[i];

        if (auto path = index.IsOurScript(scriptPubKey)) {
            // Phase M.6.2: value is already AmountUna from outputs vector
            WalletUTXO utxo(TxId(txid_uint256), static_cast<uint32_t>(i),
                           value,  // Already AmountUna
                           scriptPubKey, *path, height);
            index.AddUTXO(utxo);
        }
    }

    // Process inputs (spend existing UTXOs)
    for (const auto& [prev_txid, prev_vout] : inputs) {
        // Phase M.4.3-B Step 3: Convert hex string → uint256 → TxId
        TxId prev_txid_typed = TxId(uint256::FromHexUnsafe(prev_txid));
        if (index.IsUTXOSpent(prev_txid_typed, prev_vout)) {
            continue; // Already spent
        }
        index.SpendUTXO(prev_txid_typed, prev_vout, height);
    }
}

std::vector<uint8_t> TransactionProcessor::ParseScriptPubKey(const std::string& hex) {
    std::vector<uint8_t> script;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        script.push_back(byte);
    }
    return script;
}

bool TransactionProcessor::IsP2WPKH(const std::vector<uint8_t>& script) {
    // P2WPKH: OP_0 <20-byte-hash>
    return script.size() == 22 && script[0] == 0x00 && script[1] == 0x14;
}

bool TransactionProcessor::IsP2TR(const std::vector<uint8_t>& script) {
    // P2TR (BIP341): OP_1 <32-byte-pubkey>
    // scriptPubKey format: 0x51 0x20 <32 bytes>
    return script.size() == 34 && script[0] == 0x51 && script[1] == 0x20;
}

std::vector<uint8_t> TransactionProcessor::ExtractPubKeyHash(const std::vector<uint8_t>& script) {
    if (!IsP2WPKH(script)) {
        return {};
    }
    return std::vector<uint8_t>(script.begin() + 2, script.end());
}

std::vector<uint8_t> TransactionProcessor::ExtractTaprootPubkey(const std::vector<uint8_t>& script) {
    if (!IsP2TR(script)) {
        return {};
    }
    // Extract 32-byte x-only pubkey (skip OP_1 and push length)
    return std::vector<uint8_t>(script.begin() + 2, script.end());
}

// ============================================================================
// Phase F: Zero-Knowledge Privacy Methods
// ============================================================================

bool UTXOIndex::AddConfidentialUTXO(const ZKOutput& zk_output) {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Use INSERT OR REPLACE for idempotency
    const char* sql = R"(
        INSERT OR REPLACE INTO wallet_utxos
        (txid, vout, value, spk, path, height, spend_height, is_coinbase,
         is_confidential, commitment, range_proof, blinding_factor, nonce)
        VALUES (?, ?, ?, '', NULL, ?, NULL, 0, 1, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare add confidential UTXO statement: "
                  << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Bind parameters
    // Phase M.4: Convert TxId to hex for SQLite storage
    std::string txid_hex = zk_output.txid.AsUint256().GetHex();
    sqlite3_bind_text(stmt, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, zk_output.vout);
    // Phase M.6.2: Extract raw value for SQLite boundary
    sqlite3_bind_int64(stmt, 3, zk_output.amount.GetInt64());  // Decrypted amount
    sqlite3_bind_int(stmt, 4, zk_output.block_height);
    sqlite3_bind_blob(stmt, 5, zk_output.commitment.data(), zk_output.commitment.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 6, zk_output.range_proof.data(), zk_output.range_proof.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 7, zk_output.blinding_factor.data(), zk_output.blinding_factor.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 8, zk_output.nonce.data(), zk_output.nonce.size(), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "ERROR: Failed to add confidential UTXO: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Phase M.4: Convert TxId to hex for logging
    // Phase M.6.2: Extract raw value for logging
    std::cout << "INFO: Added confidential UTXO " << zk_output.txid.AsUint256().GetHex() << ":" << zk_output.vout
              << " with amount " << zk_output.amount.GetUna() << " una" << std::endl;

    return true;
}

std::vector<WalletUTXO> UTXOIndex::GetConfidentialUTXOs() const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    std::vector<WalletUTXO> utxos;

    // Query all unspent confidential UTXOs with all fields
    const char* sql = R"(
        SELECT txid, vout, value, spk, path, height, is_coinbase,
               commitment, range_proof, blinding_factor, nonce
        FROM wallet_utxos
        WHERE is_confidential = 1 AND spend_height IS NULL
        ORDER BY value DESC
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "ERROR: Failed to prepare get confidential UTXOs statement: "
                  << sqlite3_errmsg(db_) << std::endl;
        return utxos;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WalletUTXO utxo;
        // Phase M.0: Convert hex string from database to uint256
        std::string txid_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        utxo.txid = TxId(uint256::FromHexUnsafe(txid_hex));
        utxo.vout = sqlite3_column_int(stmt, 1);
        // Phase M.6.2: SQLite boundary - wrap value in AmountUna
        utxo.value = AmountUna::Una(static_cast<uint64_t>(sqlite3_column_int64(stmt, 2)));

        // Get scriptPubKey blob
        const void* spk_data = sqlite3_column_blob(stmt, 3);
        int spk_size = sqlite3_column_bytes(stmt, 3);
        if (spk_data && spk_size > 0) {
            utxo.spk.assign(static_cast<const uint8_t*>(spk_data),
                           static_cast<const uint8_t*>(spk_data) + spk_size);
        }

        utxo.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        utxo.height = sqlite3_column_int(stmt, 5);
        utxo.is_coinbase = sqlite3_column_int(stmt, 6) != 0;
        utxo.is_confidential = true;

        // Load commitment
        const void* commitment_data = sqlite3_column_blob(stmt, 7);
        int commitment_size = sqlite3_column_bytes(stmt, 7);
        if (commitment_data && commitment_size > 0) {
            utxo.commitment.assign(static_cast<const uint8_t*>(commitment_data),
                                  static_cast<const uint8_t*>(commitment_data) + commitment_size);
        }

        // Load range proof
        const void* proof_data = sqlite3_column_blob(stmt, 8);
        int proof_size = sqlite3_column_bytes(stmt, 8);
        if (proof_data && proof_size > 0) {
            utxo.range_proof.assign(static_cast<const uint8_t*>(proof_data),
                                   static_cast<const uint8_t*>(proof_data) + proof_size);
        }

        // Load blinding factor
        const void* blind_data = sqlite3_column_blob(stmt, 9);
        int blind_size = sqlite3_column_bytes(stmt, 9);
        if (blind_data && blind_size > 0) {
            utxo.blinding_factor.assign(static_cast<const uint8_t*>(blind_data),
                                       static_cast<const uint8_t*>(blind_data) + blind_size);
        }

        // Load nonce
        const void* nonce_data = sqlite3_column_blob(stmt, 10);
        int nonce_size = sqlite3_column_bytes(stmt, 10);
        if (nonce_data && nonce_size > 0) {
            utxo.nonce.assign(static_cast<const uint8_t*>(nonce_data),
                            static_cast<const uint8_t*>(nonce_data) + nonce_size);
        }

        // Confidential UTXOs live in a shared chainstate DB. Only expose rows
        // whose script is currently registered to this wallet, and prefer the
        // live watch-script path over any stale persisted path on the row.
        {
            std::lock_guard<std::mutex> scripts_lock(scripts_mutex_);
            auto watch_it = watched_scripts_.find(utxo.spk);
            if (watch_it == watched_scripts_.end()) {
                continue;
            }
            utxo.path = watch_it->second;
        }

        utxos.push_back(utxo);
    }

    sqlite3_finalize(stmt);
    return utxos;
}

// Phase M.6.2: Return AmountUna for type safety
AmountUna UTXOIndex::GetConfidentialBalance() const {
    uint64_t balance = 0;
    for (const auto& utxo : GetConfidentialUTXOs()) {
        balance += utxo.value.GetUna();
    }
    return AmountUna::Una(balance);
}

// Phase M.6.2: Return AmountUna for type safety
AmountUna UTXOIndex::GetTotalBalance() const {
    // Return combined transparent + confidential balance
    // Each method has its own lock, so no need for additional locking here
    // Phase M.6.3: Use checked arithmetic (overflow safe)
    auto result = GetBalance().Add(GetConfidentialBalance());
    return result.value_or(GetBalance());  // If overflow, return just transparent balance
}

std::vector<ZKOutput> UTXOIndex::ScanForNewConfidentialOutputs(
    int last_scanned_height,
    int current_height,
    const std::vector<uint8_t>& view_key) {

    // NOTE: This method is a placeholder for now.
    // The actual scanning is performed by ZKWalletSync, which:
    // 1. Queries ExplorerDB.getConfidentialOutputsInRange()
    // 2. Rewinds proofs with the view key
    // 3. Calls AddConfidentialUTXO() for discovered outputs
    //
    // This method exists for API completeness but is not currently used.
    // If needed in the future, it could query ExplorerDB directly.

    std::vector<ZKOutput> outputs;
    g_logger.warning("[UTXOIndex] ScanForNewConfidentialOutputs called but not implemented. "
                    "Use ZKWalletSync for background scanning.");
    return outputs;
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Control (Crash Safety - CRITICAL-002 fix)
// ═══════════════════════════════════════════════════════════════════════════

bool UTXOIndex::BeginTransaction() {
    std::lock_guard<std::mutex> lock(db_mutex_);

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to begin transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool UTXOIndex::CommitTransaction() {
    std::lock_guard<std::mutex> lock(db_mutex_);

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to commit transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool UTXOIndex::RollbackTransaction() {
    std::lock_guard<std::mutex> lock(db_mutex_);

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err_msg);

    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to rollback transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Database Reset (AssumeUTXO Rollback)
// ═══════════════════════════════════════════════════════════════════════════

bool UTXOIndex::ClearAll() {
    // ⚠️ DANGER: This deletes ALL UTXOs and metadata
    // Only use for AssumeUTXO rollback on validation failure

    std::lock_guard<std::mutex> lock(db_mutex_);

    g_logger.warning("[UTXOIndex] ⚠️  ClearAll() called - deleting ALL UTXOs and metadata");

    // Begin atomic transaction
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to begin transaction: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    // Delete all UTXOs
    // M.5.2 FIX: Correct table name (was "utxos", should be "wallet_utxos")
    rc = sqlite3_exec(db_, "DELETE FROM wallet_utxos", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to delete UTXOs: " + error);
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    // Delete all metadata
    rc = sqlite3_exec(db_, "DELETE FROM utxo_metadata", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to delete metadata: " + error);
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    // Commit transaction
    rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown error";
        g_logger.error("[UTXOIndex] Failed to commit ClearAll: " + error);
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    g_logger.info("[UTXOIndex] ✓ Successfully cleared all UTXOs and metadata");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Metadata Storage (Crash Safety - CRITICAL-003 fix)
// ═══════════════════════════════════════════════════════════════════════════

bool UTXOIndex::SetMetadata(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    const char* sql = "INSERT OR REPLACE INTO utxo_metadata (key, value) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("[UTXOIndex] Failed to prepare SetMetadata statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        g_logger.error("[UTXOIndex] Failed to set metadata '" + key + "': " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    return true;
}

std::optional<std::string> UTXOIndex::GetMetadata(const std::string& key) const {
    std::lock_guard<std::mutex> lock(db_mutex_);

    const char* sql = "SELECT value FROM utxo_metadata WHERE key = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("[UTXOIndex] Failed to prepare GetMetadata statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    std::optional<std::string> result;

    if (rc == SQLITE_ROW) {
        const unsigned char* value = sqlite3_column_text(stmt, 0);
        if (value) {
            result = std::string(reinterpret_cast<const char*>(value));
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool UTXOIndex::DeleteMetadata(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    const char* sql = "DELETE FROM utxo_metadata WHERE key = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("[UTXOIndex] Failed to prepare DeleteMetadata statement: " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        g_logger.error("[UTXOIndex] Failed to delete metadata '" + key + "': " +
                      std::string(sqlite3_errmsg(db_)));
        return false;
    }

    return true;
}

// Phase 11a: Get Utreexo position for proof generation
std::optional<uint64_t> UTXOIndex::getUtreexoPosition(const TxId& txid, uint32_t vout) const {
    // ✅ LOCK: Protect all SQLite operations (statements not thread-safe)
    std::lock_guard<std::mutex> lock(db_mutex_);

    if (!stmt_get_position_) {
        g_logger.error("[UTXOIndex] getUtreexoPosition: stmt_get_position_ not prepared");
        return std::nullopt;
    }

    sqlite3_reset(stmt_get_position_);

    // Bind parameters
    std::string txid_hex = txid.AsUint256().GetHex();
    sqlite3_bind_text(stmt_get_position_, 1, txid_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_get_position_, 2, vout);

    int rc = sqlite3_step(stmt_get_position_);
    if (rc == SQLITE_ROW) {
        // Check if utreexo_position is NULL
        if (sqlite3_column_type(stmt_get_position_, 0) == SQLITE_NULL) {
            return std::nullopt;  // Position not tracked for this UTXO
        }

        uint64_t position = static_cast<uint64_t>(sqlite3_column_int64(stmt_get_position_, 0));
        return position;
    }

    // UTXO not found or error
    if (rc != SQLITE_DONE) {
        g_logger.error("[UTXOIndex] getUtreexoPosition query failed: " +
                      std::string(sqlite3_errmsg(db_)));
    }

    return std::nullopt;
}

} // namespace dinero
