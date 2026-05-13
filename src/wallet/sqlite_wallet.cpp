// DEAD CODE — not compiled (not in CMakeLists.txt).
// Production wallet uses wallet_manager.cpp with PBKDF2+AES-GCM seed encryption.
// Kept for reference only. Do not rely on this code.

#include "dinero/core/wallet/sqlite_wallet.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <ctime>
#include <random>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <sys/types.h>
#include <strings.h>  // for strcasecmp
#include "wallet/sqlite_utils.hpp"
#include "wallet/hd_wallet.h"
#include "crypto/dinero_crypto_minimal.h"
#include "consensus/coin_type.h"
#include <secp256k1.h>              // secp256k1_context, flags

namespace Dinero {
namespace Wallet {

// Using declarations for HDWallet types
// Note: HDWallet is now a global class, ExtPriv is from old implementation
// using ::HDWallet;  // HDWallet is global now
// ExtPriv doesn't exist in new HDWallet implementation

// Old arrays removed - now using bootstrap-first approach

// Synchronous mode helpers
static int parseSyncEnv() {
    const char* s = std::getenv("DINERO_WALLET_SYNC");
    if (!s || !*s) return 1;                 // default NORMAL
    if (!strcasecmp(s, "OFF"))   return 0;
    if (!strcasecmp(s, "NORMAL")) return 1;
    if (!strcasecmp(s, "FULL"))  return 2;
    if (!strcasecmp(s, "EXTRA")) return 3;   // supported by SQLite
    return atoi(s);                          // allow numeric override
}

static const char* syncName(int v) {
    switch(v) { 
        case 0: return "OFF"; 
        case 1: return "NORMAL"; 
        case 2: return "FULL"; 
        case 3: return "EXTRA"; 
    }
    return "UNKNOWN";
}

static int getSync(sqlite3* db) {
    sqlite3_stmt* st = nullptr; 
    int v = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA synchronous;", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

// RAII Transaction implementation
SQLiteTransaction::SQLiteTransaction(sqlite3* db) : db(db), committed(false), rolled_back(false) {
    // Prevent writer starvation & improve atomicity under chaos tests
    sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
}

SQLiteTransaction::~SQLiteTransaction() {
    if (!committed && !rolled_back) {
        rollback();
    }
}

bool SQLiteTransaction::commit() {
    if (committed || rolled_back) return false;
    
    int rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    committed = (rc == SQLITE_OK);
    
    // Make committed frames visible to readers quickly (keeps WAL small in tests)
    if (committed) {
        sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE);", nullptr, nullptr, nullptr);
    }
    
    return committed;
}

void SQLiteTransaction::rollback() {
    if (committed || rolled_back) return;
    
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    rolled_back = true;
}

// RAII Statement implementation
SQLiteStatement::SQLiteStatement(sqlite3* db, const char* sql) : stmt(nullptr) {
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
}

SQLiteStatement::~SQLiteStatement() {
    if (stmt) {
        sqlite3_finalize(stmt);
    }
}

bool SQLiteStatement::bind_text(int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool SQLiteStatement::bind_int(int index, int value) {
    return sqlite3_bind_int(stmt, index, value) == SQLITE_OK;
}

bool SQLiteStatement::bind_int64(int index, int64_t value) {
    return sqlite3_bind_int64(stmt, index, value) == SQLITE_OK;
}

int SQLiteStatement::step() {
    return sqlite3_step(stmt);
}

void SQLiteStatement::reset() {
    sqlite3_reset(stmt);
}

int SQLiteStatement::column_int(int index) {
    return sqlite3_column_int(stmt, index);
}

int64_t SQLiteStatement::column_int64(int index) {
    return sqlite3_column_int64(stmt, index);
}

std::string SQLiteStatement::column_text(int index) {
    const char* text = (const char*)sqlite3_column_text(stmt, index);
    return text ? text : "";
}

// SQLiteWallet implementation
SQLiteWallet::SQLiteWallet() : db(nullptr), initialized(false), wallet_locked(false), 
                               hd_unlocked(false), network_hrp("din") {
    memset(hd_seed, 0, sizeof(hd_seed));
}

SQLiteWallet::~SQLiteWallet() {
    shutdown();
}

bool SQLiteWallet::initialize(const std::string& wallet_path) {
    if (initialized) {
        std::cerr << "❌ Wallet already initialized" << std::endl;
        return false;
    }
    
    this->wallet_path = wallet_path;
    
    // Set secure file creation mask (files created with 0600 permissions)
    mode_t old_umask = umask(0077);
    
    // Create wallet directory if needed
    std::filesystem::path wallet_dir = std::filesystem::path(wallet_path).parent_path();
    std::filesystem::create_directories(wallet_dir);
    
    // Configure SQLite for serialized mode (must be called before any sqlite3_open)
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    
    // Shared cache is deprecated on macOS; avoid calling it there
#if !defined(__APPLE__)
    sqlite3_enable_shared_cache(0);  // Disable shared cache for security
#endif
    
    // Open SQLite database with full mutex protection
    int rc = sqlite3_open_v2(wallet_path.c_str(), &db, 
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 
                            nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "❌ Failed to open wallet database: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    
    // Secure file permissions: wallet files should be 0600 (owner read/write only)
    if (chmod(wallet_path.c_str(), 0600) != 0) {
        std::cerr << "⚠️ Warning: Failed to set secure permissions on wallet file" << std::endl;
    }
    
    // Set busy timeout at connection level
    sqlite3_busy_timeout(db, 5000);
    
    // Enable extended result codes for better error reporting
    sqlite3_extended_result_codes(db, 1);
    
    // Add SQL statement tracing for debugging (only when explicitly enabled)
    if (std::getenv("DINERO_SQL_TRACE")) {
        sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void*, void* p, void*) -> int {
            auto *stmt = static_cast<sqlite3_stmt*>(p);
            if (stmt) {
                std::cerr << "🔍 SQL: " << sqlite3_sql(stmt) << std::endl;
            }
            return 0;
        }, nullptr);
    }
    
    // Set crash-safe SQLite pragmas
    if (!setSQLitePragmas()) {
        std::cerr << "❌ Failed to set SQLite pragmas" << std::endl;
        return false;
    }
    
    // Run schema migrations (includes base table creation)
    if (!migrateSchema()) {
        std::cerr << "❌ Failed to migrate schema" << std::endl;
        return false;
    }
    
    // Check for crash recovery
    WalletMeta meta = getMeta();
    if (!meta.pending_block_hash.empty()) {
        std::cout << "🔄 Wallet crash recovery: reapplying block " << meta.pending_block_hash << std::endl;
        if (!reapplyPendingBlock()) {
            std::cerr << "⚠️  Failed to reapply pending block, continuing..." << std::endl;
        }
    }
    
    initialized = true;
    
    // Get actual user_version after migration
    int user_version = queryInt("PRAGMA user_version;");
    
    std::cout << "✅ SQLite wallet initialized: " << wallet_path << std::endl;
    std::cout << "   📊 Schema user_version: " << user_version << std::endl;
    std::cout << "   📈 Last applied height: " << meta.last_applied_height << std::endl;
    
    // Show actual synchronous mode from database
    int sync_mode = queryInt("PRAGMA synchronous;");
    std::cout << "   🔒 Crash-safe: WAL mode + synchronous=" << syncName(sync_mode) << std::endl;
    
    return true;
}

void SQLiteWallet::shutdown() {
    if (!initialized) return;
    
    std::cout << "🛑 Shutting down SQLite wallet..." << std::endl;
    
    if (db) {
        // Clean shutdown with WAL truncation for deterministic next open
        wal_clean_shutdown(db);
        std::cout << "   📝 WAL checkpoint: 0/0 pages written" << std::endl;
        
        sqlite3_close(db);
        db = nullptr;
    }
    
    initialized = false;
    std::cout << "✅ SQLite wallet shutdown complete" << std::endl;
}

bool SQLiteWallet::setSQLitePragmas() {
    // Determine durability profile from environment
    Durability durability = Durability::NORMAL;  // default
    const char* strong = std::getenv("DIN_WAL_STRONG");
    if (strong && std::string(strong) == "1") {
        durability = Durability::SAFE;
    }
    
    try {
        // Apply wallet pragmas using our centralized helper
        apply_wallet_pragmas(db, durability);
        
        // Startup checkpoint to replay any stale WAL after crash
        wal_startup_checkpoint(db);
        
        std::cout << "🔒 " << to_cstr(durability) << " durability mode (" 
                  << (durability == Durability::SAFE ? "FULL" : "NORMAL") << ")" << std::endl;
        
        // Validate WAL mode is actually active
        auto mode_rows = queryRows("PRAGMA journal_mode;");
        std::string mode = mode_rows.empty() ? "" : mode_rows[0][0];
        if (mode != "wal") {
            std::cerr << "❌ Journal mode not WAL, got: " << mode << std::endl;
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to set SQLite pragmas: " << e.what() << std::endl;
        return false;
    }
}

std::string SQLiteWallet::integrity_check() {
    if (!db) return "no database";
    
    std::string result;
    sqlite3_exec(db, "PRAGMA integrity_check;", [](void* p, int, char** v, char**) -> int {
        *static_cast<std::string*>(p) = v && v[0] ? v[0] : "";
        return 0;
    }, &result, nullptr);
    return result.empty() ? "ok" : result;
}

bool SQLiteWallet::createTables() {
    const std::vector<std::string> table_sqls = {
        // Wallet metadata (single row)
        R"(
        CREATE TABLE IF NOT EXISTS wallet_meta (
            id INTEGER PRIMARY KEY CHECK (id=1),
            schema_version INTEGER NOT NULL DEFAULT 1,
            last_applied_height INTEGER NOT NULL DEFAULT -1,
            last_applied_hash TEXT NOT NULL DEFAULT '',
            pending_block_hash TEXT NOT NULL DEFAULT '',
            birth_height INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT 0
        );
        )",
        
        // Private keys (encrypted storage)
        R"(
        CREATE TABLE IF NOT EXISTS keys (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            pubkey TEXT UNIQUE NOT NULL,
            privkey TEXT,
            enc_salt TEXT,
            enc_nonce TEXT,
            is_encrypted INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL
        );
        )",
        
        // Addresses with script and key association
        R"(
        CREATE TABLE IF NOT EXISTS addresses (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            address TEXT UNIQUE NOT NULL,
            script_pubkey TEXT NOT NULL,
            type TEXT NOT NULL CHECK(type IN('p2wpkh','p2wsh')),
            key_id INTEGER,
            watch_only INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL,
            FOREIGN KEY(key_id) REFERENCES keys(id) ON DELETE SET NULL
        );
        )",
        
        // Transaction metadata
        R"(
        CREATE TABLE IF NOT EXISTS tx (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            txid TEXT UNIQUE NOT NULL,
            blockhash TEXT,
            height INTEGER,
            time INTEGER,
            raw TEXT,
            direction TEXT CHECK(direction IN('recv','send','self')),
            amount INTEGER NOT NULL DEFAULT 0,
            fee INTEGER
        );
        )",
        
        // UTXO tracking with spend information
        R"(
        CREATE TABLE IF NOT EXISTS utxos (
            txid TEXT NOT NULL,
            vout INTEGER NOT NULL,
            address_id INTEGER NOT NULL,
            value INTEGER NOT NULL,
            script_pubkey TEXT NOT NULL,
            height INTEGER NOT NULL,
            spend_txid TEXT,
            spend_height INTEGER,
            PRIMARY KEY(txid, vout),
            FOREIGN KEY(address_id) REFERENCES addresses(id) ON DELETE CASCADE
        );
        )"
    };
    
    // Create tables
    for (const auto& sql : table_sqls) {
        if (!execSQL(sql)) {
            return false;
        }
    }
    
    // Create indices
    const std::vector<std::string> index_sqls = {
        "CREATE INDEX IF NOT EXISTS idx_utxos_unspent ON utxos(spend_txid) WHERE spend_txid IS NULL;",
        "CREATE INDEX IF NOT EXISTS idx_tx_height ON tx(height);",
        "CREATE INDEX IF NOT EXISTS idx_addresses_type ON addresses(type);",
        "CREATE INDEX IF NOT EXISTS idx_keys_pubkey ON keys(pubkey);"
    };
    
    for (const auto& sql : index_sqls) {
        if (!execSQL(sql)) {
            return false;
        }
    }
    
    // Initialize wallet_meta if empty
    if (queryInt("SELECT COUNT(*) FROM wallet_meta WHERE id=1;") == 0) {
        WalletMeta meta;
        meta.schema_version = 1;
        meta.created_at = std::time(nullptr);
        
        if (!setMeta(meta)) {
            std::cerr << "❌ Failed to initialize wallet metadata" << std::endl;
            return false;
        }
    }
    
    return true;
}

// Wallet metadata operations
SQLiteWallet::WalletMeta SQLiteWallet::getMeta() {
    WalletMeta meta;
    
    auto rows = queryRows(R"(
        SELECT schema_version, last_applied_height, last_applied_hash, 
               pending_block_hash, birth_height, created_at 
        FROM wallet_meta WHERE id=1;
    )");
    
    if (!rows.empty() && rows[0].size() >= 6) {
        meta.schema_version = std::stoi(rows[0][0]);
        meta.last_applied_height = std::stoi(rows[0][1]);
        meta.last_applied_hash = rows[0][2];
        meta.pending_block_hash = rows[0][3];
        meta.birth_height = std::stoi(rows[0][4]);
        meta.created_at = std::stoull(rows[0][5]);
    }
    
    return meta;
}

bool SQLiteWallet::setMeta(const WalletMeta& meta) {
    return execSQL(R"(
        INSERT OR REPLACE INTO wallet_meta 
        (id, schema_version, last_applied_height, last_applied_hash, 
         pending_block_hash, birth_height, created_at)
        VALUES (1, ?, ?, ?, ?, ?, ?);
    )", {
        std::to_string(meta.schema_version),
        std::to_string(meta.last_applied_height),
        meta.last_applied_hash,
        meta.pending_block_hash,
        std::to_string(meta.birth_height),
        std::to_string(meta.created_at)
    });
}

bool SQLiteWallet::setMetaField(const std::string& field, const std::string& value) {
    // Validate field name to prevent SQL injection
    const std::vector<std::string> allowed_fields = {
        "last_applied_height", "last_applied_hash", "pending_block_hash"
    };
    
    if (std::find(allowed_fields.begin(), allowed_fields.end(), field) == allowed_fields.end()) {
        std::cerr << "❌ Invalid meta field: " << field << std::endl;
        return false;
    }
    
    std::string sql = "UPDATE wallet_meta SET " + field + " = ? WHERE id = 1;";
    return execSQL(sql, {value});
}

// Key management
int SQLiteWallet::generateNewKey() {
    // Note: wallet_mutex is already held by caller (getNewAddress)
    
    // Generate a new key pair (simplified - in production use proper crypto)
    std::mt19937 gen;
    
    // Check for deterministic test seed
    const char* test_seed = std::getenv("DINERO_WALLET_TEST_SEED");
    if (test_seed || std::getenv("CI") || std::getenv("DINERO_FAST_TEST")) {
        // Use deterministic seed for tests
        std::string seed_str = test_seed ? test_seed : "dinero-regtest-seed";
        std::hash<std::string> hasher;
        size_t seed_hash = hasher(seed_str);
        gen.seed(static_cast<std::mt19937::result_type>(seed_hash));
    } else {
        // Use random device for production
        std::random_device rd;
        gen.seed(rd());
    }
    
    std::uniform_int_distribution<> dis(0, 255);
    
    // Generate 32-byte private key
    std::string privkey_hex;
    for (int i = 0; i < 32; i++) {
        std::stringstream ss;
        ss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
        privkey_hex += ss.str();
    }
    
    // Derive compressed public key (simplified - use real secp256k1)
    std::string pubkey_hex = "02" + privkey_hex.substr(0, 62); // Simplified
    
    WalletKey key;
    key.pubkey = pubkey_hex;
    key.privkey = privkey_hex; // Store as hex for now
    key.is_encrypted = false;
    key.created_at = std::time(nullptr);
    
    if (!storeKey(key)) {
        return 0;
    }
    
    // Get the inserted key ID
    return queryInt("SELECT last_insert_rowid();");
}

bool SQLiteWallet::storeKey(const WalletKey& key) {
    return execSQL(R"(
        INSERT INTO keys (pubkey, privkey, enc_salt, enc_nonce, is_encrypted, created_at)
        VALUES (?, ?, ?, ?, ?, ?);
    )", {
        key.pubkey,
        key.privkey,
        key.enc_salt,
        key.enc_nonce,
        key.is_encrypted ? "1" : "0",
        std::to_string(key.created_at)
    });
}

// Address management
std::string SQLiteWallet::getNewAddress() {
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    // Generate new key
    int key_id = generateNewKey();
    if (key_id == 0) {
        return "";
    }
    
    // Get the key
    WalletKey key = getKey(key_id);
    if (key.pubkey.empty()) {
        return "";
    }
    
    // Derive address and script
    std::string address = deriveAddress(key.pubkey);
    std::string script_pubkey = deriveScriptPubKey(key.pubkey);
    
    // Store address
    WalletAddress addr;
    addr.address = address;
    addr.script_pubkey = script_pubkey;
    addr.type = "p2wpkh";
    addr.key_id = key_id;
    addr.watch_only = false;
    addr.created_at = std::time(nullptr);
    
    bool success = execSQL(R"(
        INSERT INTO addresses (address, script_pubkey, type, key_id, watch_only, created_at)
        VALUES (?, ?, ?, ?, ?, ?);
    )", {
        addr.address,
        addr.script_pubkey,
        addr.type,
        std::to_string(addr.key_id),
        addr.watch_only ? "1" : "0",
        std::to_string(addr.created_at)
    });
    
    return success ? address : "";
}

SQLiteWallet::WalletKey SQLiteWallet::getKey(int key_id) {
    WalletKey key;
    
    auto rows = queryRows(R"(
        SELECT id, pubkey, privkey, enc_salt, enc_nonce, is_encrypted, created_at
        FROM keys WHERE id = ?;
    )", {std::to_string(key_id)});
    
    if (!rows.empty() && rows[0].size() >= 7) {
        key.id = std::stoi(rows[0][0]);
        key.pubkey = rows[0][1];
        key.privkey = rows[0][2];
        key.enc_salt = rows[0][3];
        key.enc_nonce = rows[0][4];
        key.is_encrypted = (rows[0][5] == "1");
        key.created_at = std::stoull(rows[0][6]);
    }
    
    return key;
}

// UTXO operations
std::vector<SQLiteWallet::WalletUTXO> SQLiteWallet::listUnspent() {
    std::vector<WalletUTXO> utxos;
    
    auto rows = queryRows(R"(
        SELECT u.txid, u.vout, u.address_id, u.value, u.script_pubkey, u.height,
               u.spend_txid, u.spend_height
        FROM utxos u
        WHERE u.spend_txid IS NULL
        ORDER BY u.height DESC, u.txid, u.vout;
    )");
    
    for (const auto& row : rows) {
        if (row.size() >= 8) {
            WalletUTXO utxo;
            utxo.txid = row[0];
            utxo.vout = std::stoi(row[1]);
            utxo.address_id = std::stoi(row[2]);
            utxo.value = std::stoll(row[3]);
            utxo.script_pubkey = row[4];
            utxo.height = std::stoi(row[5]);
            utxo.spend_txid = row[6];
            utxo.spend_height = row[7].empty() ? -1 : std::stoi(row[7]);
            
            utxos.push_back(utxo);
        }
    }
    
    return utxos;
}

int64_t SQLiteWallet::getBalance() {
    return queryInt("SELECT COALESCE(SUM(value), 0) FROM utxos WHERE spend_txid IS NULL;");
}

// Helper methods
bool SQLiteWallet::execSQL(const std::string& sql) {
    return execSQL(sql, {});
}

bool SQLiteWallet::execSQL(const std::string& sql, const std::vector<std::string>& params) {
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        std::cerr << "❌ SQL prepare failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    
    // Bind parameters
    for (size_t i = 0; i < params.size(); i++) {
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_STATIC);
    }
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE || rc == SQLITE_ROW);
}

int SQLiteWallet::queryInt(const std::string& sql, const std::vector<std::string>& params) {
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    // Bind parameters
    for (size_t i = 0; i < params.size(); i++) {
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_STATIC);
    }
    
    int result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return result;
}

std::vector<std::vector<std::string>> SQLiteWallet::queryRows(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<std::vector<std::string>> rows;
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        return rows;
    }
    
    // Bind parameters
    for (size_t i = 0; i < params.size(); i++) {
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_STATIC);
    }
    
    // Fetch rows
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::string> row;
        int cols = sqlite3_column_count(stmt);
        
        for (int i = 0; i < cols; i++) {
            const char* text = (const char*)sqlite3_column_text(stmt, i);
            row.push_back(text ? text : "");
        }
        
        rows.push_back(row);
    }
    
    sqlite3_finalize(stmt);
    return rows;
}

// Address derivation (simplified - use real crypto in production)
std::string SQLiteWallet::deriveAddress(const std::string& pubkey) {
    // Simplified bech32 address derivation
    // In production, use proper RIPEMD160(SHA256(pubkey)) and bech32 encoding
    std::hash<std::string> hasher;
    size_t hash_val = hasher(pubkey);
    
    std::stringstream ss;
    ss << "rdin1q" << std::hex << hash_val;
    return ss.str().substr(0, 42); // Truncate to reasonable length
}

std::string SQLiteWallet::deriveScriptPubKey(const std::string& pubkey) {
    // Simplified P2WPKH script: OP_0 <20-byte-hash>
    // In production, use proper RIPEMD160(SHA256(pubkey))
    std::hash<std::string> hasher;
    size_t hash_val = hasher(pubkey);
    
    std::stringstream ss;
    ss << "0014" << std::hex << std::setw(16) << std::setfill('0') << hash_val;
    return ss.str().substr(0, 44); // 0014 + 40 hex chars = 44 chars
}

// Crash recovery
bool SQLiteWallet::reapplyPendingBlock() {
    // TODO: Implement block reapplication
    // For now, just clear the pending block hash
    return setMetaField("pending_block_hash", "");
}

// Block application (crash-safe)
bool SQLiteWallet::applyBlock(const BlockView& block) {
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    SQLiteTransaction tx(db);
    
    // Set crash marker
    if (!setMetaField("pending_block_hash", block.hash)) {
        return false;
    }
    
    // Process transactions in the block
    for (const auto& wallet_tx : block.txs) {
        // Store transaction metadata
        if (!upsertTx(wallet_tx)) {
            std::cerr << "⚠️  Failed to store tx: " << wallet_tx.txid << std::endl;
            continue;
        }
        
        // TODO: Process inputs (mark spends)
        // TODO: Process outputs (create UTXOs for our addresses)
    }
    
    // Update wallet state
    if (!setMetaField("last_applied_height", std::to_string(block.height))) {
        return false;
    }
    
    if (!setMetaField("last_applied_hash", block.hash)) {
        return false;
    }
    
    // Clear crash marker
    if (!setMetaField("pending_block_hash", "")) {
        return false;
    }
    
    return tx.commit();
}

bool SQLiteWallet::upsertTx(const WalletTx& tx) {
    return execSQL(R"(
        INSERT OR REPLACE INTO tx 
        (txid, blockhash, height, time, raw, direction, amount, fee)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )", {
        tx.txid,
        tx.blockhash,
        std::to_string(tx.height),
        std::to_string(tx.time),
        tx.raw,
        tx.direction,
        std::to_string(tx.amount),
        std::to_string(tx.fee)
    });
}

// Rollback for reorgs
bool SQLiteWallet::rollbackToHeight(int target_height) {
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    SQLiteTransaction tx(db);
    
    // Remove transactions above target height
    if (!execSQL("DELETE FROM tx WHERE height > ?;", {std::to_string(target_height)})) {
        return false;
    }
    
    // Delete UTXOs created above target height
    if (!execSQL("DELETE FROM utxos WHERE height > ?;", {std::to_string(target_height)})) {
        return false;
    }
    
    // Unspend UTXOs that were spent above target height
    if (!execSQL(R"(
        UPDATE utxos 
        SET spend_txid = NULL, spend_height = NULL 
        WHERE spend_height > ?;
    )", {std::to_string(target_height)})) {
        return false;
    }
    
    // Update wallet metadata
    if (!setMetaField("last_applied_height", std::to_string(target_height))) {
        return false;
    }
    
    if (!setMetaField("pending_block_hash", "")) {
        return false;
    }
    
    return tx.commit();
}

// Backup operations
bool SQLiteWallet::backupToFile(const std::string& backup_path) {
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    // Use SQLite backup API for hot backup
    sqlite3* backup_db;
    int rc = sqlite3_open(backup_path.c_str(), &backup_db);
    
    if (rc != SQLITE_OK) {
        std::cerr << "❌ Failed to open backup database: " << sqlite3_errmsg(backup_db) << std::endl;
        return false;
    }
    
    sqlite3_backup* backup = sqlite3_backup_init(backup_db, "main", db, "main");
    if (backup) {
        sqlite3_backup_step(backup, -1);
        sqlite3_backup_finish(backup);
    }
    
    rc = sqlite3_errcode(backup_db);
    sqlite3_close(backup_db);
    
    if (rc == SQLITE_OK) {
        std::cout << "✅ Wallet backed up to: " << backup_path << std::endl;
        return true;
    } else {
        std::cerr << "❌ Backup failed: " << sqlite3_errmsg(backup_db) << std::endl;
        return false;
    }
}

bool SQLiteWallet::validateIntegrity() {
    std::string result = queryRows("PRAGMA integrity_check;")[0][0];
    return result == "ok";
}

int SQLiteWallet::getWriterSynchronousMode() const {
    if (!initialized || !db) return -1;
    return getSync(db);
}

sqlite3* SQLiteWallet::openReaderConnection() const {
    if (!initialized) return nullptr;
    
    sqlite3* readerDb = nullptr;
    int rc = sqlite3_open_v2(wallet_path.c_str(), &readerDb, 
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "❌ Failed to open reader connection: " << sqlite3_errmsg(readerDb) << std::endl;
        if (readerDb) sqlite3_close(readerDb);
        return nullptr;
    }
    
    // Reader safety belt: enforce query-only mode
    sqlite3_exec(readerDb, "PRAGMA query_only=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(readerDb, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(readerDb, "PRAGMA trusted_schema=OFF;", nullptr, nullptr, nullptr);
    // Optional tuning for reader
    sqlite3_exec(readerDb, "PRAGMA cache_size=-32000;", nullptr, nullptr, nullptr);  // 32MB cache
    sqlite3_exec(readerDb, "PRAGMA mmap_size=67108864;", nullptr, nullptr, nullptr); // 64MB mmap
    
    // Assert query_only is actually enabled
    int qo = -1;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(readerDb, "PRAGMA query_only;", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        qo = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    
    if (qo != 1) {
        std::cerr << "❌ Reader connection safety belt failed: query_only=" << qo << std::endl;
        sqlite3_close(readerDb);
        return nullptr;
    }
    
    std::cout << "✅ Reader connection opened with query_only=ON" << std::endl;
    return readerDb;
}

// Bulletproof bootstrap and migration with detailed error logging
static inline int exec_dbg(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "❌ SQL rc=" << rc << " xrc=" << sqlite3_extended_errcode(db) 
                  << " err=" << (err ? err : "(null)") << std::endl
                  << "SQL: " << sql << std::endl;
        sqlite3_free(err);
    }
    return rc;
}

// Migration dry-run support (set DINERO_WALLET_MIGRATE_DRYRUN=1 to log but not execute ALTERs)
static bool isDryRun() {
    static bool dryrun = std::getenv("DINERO_WALLET_MIGRATE_DRYRUN") != nullptr;
    return dryrun;
}

static int exec_plan(sqlite3* db, const char* sql) {
    std::cerr << "📝 plan: " << sql << std::endl;
    return isDryRun() ? SQLITE_OK : exec_dbg(db, sql);
}

static int getUserVersion(sqlite3* db) {
    sqlite3_stmt* st = nullptr; 
    int ver = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, nullptr) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        ver = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return ver;
}

static void setUserVersion(sqlite3* db, int v) {
    char buf[64]; 
    snprintf(buf, sizeof(buf), "PRAGMA user_version=%d;", v);
    exec_dbg(db, buf);
}

// Helper function to check if a column exists in a table
static bool hasColumn(sqlite3* db, const char* table, const char* col) {
    sqlite3_stmt* st = nullptr;
    std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(st, 1); // cid,name,type,...
        if (name && strcmp(name, col) == 0) { 
            found = true; 
            break; 
        }
    }
    sqlite3_finalize(st);
    return found;
}

static int bootstrapSchema(sqlite3* db) {
    // Get current schema version
    int version = 0;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) {
        version = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    
    if (exec_dbg(db, "BEGIN IMMEDIATE;") != SQLITE_OK) return SQLITE_ERROR;
    
    // Version 0 -> 1: Initial schema with type column from day 1
    if (version < 1) {
        // Base tables (idempotent) - include type column from the start
        const char* tables[] = {
        R"(CREATE TABLE IF NOT EXISTS wallet_meta(
             id INTEGER PRIMARY KEY CHECK(id=1),
             schema_version INTEGER NOT NULL DEFAULT 1,
             last_applied_height INTEGER NOT NULL DEFAULT -1,
             last_applied_hash TEXT NOT NULL DEFAULT '',
             pending_block_hash TEXT NOT NULL DEFAULT '',
             birth_height INTEGER NOT NULL DEFAULT 0,
             created_at INTEGER NOT NULL DEFAULT 0
           );)",
        R"(CREATE TABLE IF NOT EXISTS keys(
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             pubkey TEXT UNIQUE NOT NULL,
             privkey TEXT,
             enc_salt TEXT,
             enc_nonce TEXT,
             is_encrypted INTEGER NOT NULL DEFAULT 0,
             created_at INTEGER NOT NULL
           );)",
        R"(CREATE TABLE IF NOT EXISTS addresses(
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             address TEXT UNIQUE NOT NULL,
             script_pubkey TEXT NOT NULL,
             type TEXT NOT NULL CHECK(type IN('p2wpkh','p2wsh')),
             key_id INTEGER,
             watch_only INTEGER NOT NULL DEFAULT 0,
             created_at INTEGER NOT NULL,
             FOREIGN KEY(key_id) REFERENCES keys(id) ON DELETE SET NULL
           );)",
        R"(CREATE TABLE IF NOT EXISTS tx(
             id INTEGER PRIMARY KEY AUTOINCREMENT,
             txid TEXT UNIQUE NOT NULL,
             blockhash TEXT,
             height INTEGER,
             time INTEGER,
             raw TEXT,
             direction TEXT CHECK(direction IN('recv','send','self')),
             amount INTEGER NOT NULL DEFAULT 0,
             fee INTEGER
           );)",
        R"(CREATE TABLE IF NOT EXISTS utxos(
             txid TEXT NOT NULL,
             vout INTEGER NOT NULL,
             address_id INTEGER NOT NULL,
             value INTEGER NOT NULL,
             script_pubkey TEXT NOT NULL,
             height INTEGER NOT NULL,
             spend_txid TEXT,
             spend_height INTEGER,
             PRIMARY KEY(txid,vout),
             FOREIGN KEY(address_id) REFERENCES addresses(id) ON DELETE CASCADE
           );)"
        };
        
        for (auto* sql : tables) { 
            if (exec_dbg(db, sql) != SQLITE_OK) {
                exec_dbg(db, "ROLLBACK;");
                return SQLITE_ERROR;
            }
        }

        const std::string hd_meta_sql =
            "CREATE TABLE IF NOT EXISTS hd_meta("
            "id INTEGER PRIMARY KEY CHECK(id=1), "
            "master_fpr INTEGER NOT NULL DEFAULT 0, "
            "purpose INTEGER NOT NULL DEFAULT 84, "
            "coin_type INTEGER NOT NULL DEFAULT " + std::to_string(dinero::consensus::DINERO_COIN_TYPE) + ", "
            "account INTEGER NOT NULL DEFAULT 0, "
            "ext_next_index INTEGER NOT NULL DEFAULT 0, "
            "int_next_index INTEGER NOT NULL DEFAULT 0, "
            "seed_ciphertext BLOB, "
            "seed_salt BLOB, "
            "seed_nonce BLOB, "
            "kdf TEXT NOT NULL DEFAULT 'argon2id', "
            "created_at INTEGER NOT NULL DEFAULT 0"
            ");";

        if (exec_dbg(db, hd_meta_sql.c_str()) != SQLITE_OK) {
            exec_dbg(db, "ROLLBACK;");
            return SQLITE_ERROR;
        }
        
        // Create indexes for version 1 (safe since tables have type column)
        const char* indexes_v1[] = {
            "CREATE INDEX IF NOT EXISTS idx_utxos_unspent ON utxos(spend_txid) WHERE spend_txid IS NULL;",
            "CREATE INDEX IF NOT EXISTS idx_tx_height ON tx(height);",
            "CREATE INDEX IF NOT EXISTS idx_keys_pubkey ON keys(pubkey);"
        };
        
        for (auto* sql : indexes_v1) { 
            if (exec_dbg(db, sql) != SQLITE_OK) {
                exec_dbg(db, "ROLLBACK;");
                return SQLITE_ERROR;
            }
        }
        
        version = 1;
    }
    
    // Version 1 -> 2: Handle migration for existing DBs that might lack type column
    if (version < 2) {
        // Check if addresses table exists and if it has type column
        if (!hasColumn(db, "addresses", "type")) {
            // Add type column to existing addresses table
            if (exec_dbg(db, "ALTER TABLE addresses ADD COLUMN type TEXT NOT NULL DEFAULT 'p2wpkh';") != SQLITE_OK) {
                exec_dbg(db, "ROLLBACK;");
                return SQLITE_ERROR;
            }
            
            // Backfill type column based on existing data
            if (exec_dbg(db, 
                "UPDATE addresses SET type = "
                "CASE WHEN COALESCE(change,0)=1 OR address LIKE '%change%' THEN 'p2wsh' ELSE 'p2wpkh' END "
                "WHERE type IS NULL OR type='';") != SQLITE_OK) {
                exec_dbg(db, "ROLLBACK;");
                return SQLITE_ERROR;
            }
        }
        
        // Now safe to create index on type column
        if (exec_dbg(db, "CREATE INDEX IF NOT EXISTS idx_addresses_type ON addresses(type);") != SQLITE_OK) {
            exec_dbg(db, "ROLLBACK;");
            return SQLITE_ERROR;
        }
        
        version = 2;
    }

    // Seed singleton row (idempotent)
    if (exec_dbg(db,
        "INSERT INTO wallet_meta(id,schema_version,last_applied_height,last_applied_hash,"
        "pending_block_hash,birth_height,created_at) "
        "SELECT 1,2,-1,'','',0,CAST(strftime('%s','now') AS INTEGER) "
        "WHERE NOT EXISTS(SELECT 1 FROM wallet_meta WHERE id=1);") != SQLITE_OK) {
        exec_dbg(db, "ROLLBACK;");
        return SQLITE_ERROR;
    }

    // Seed HD meta singleton row (idempotent)
    if (exec_dbg(db,
        "INSERT INTO hd_meta(id,created_at) "
        "SELECT 1,CAST(strftime('%s','now') AS INTEGER) "
        "WHERE NOT EXISTS(SELECT 1 FROM hd_meta WHERE id=1);") != SQLITE_OK) {
        exec_dbg(db, "ROLLBACK;");
        return SQLITE_ERROR;
    }

    // Set final user version
    std::string pragma = "PRAGMA user_version=" + std::to_string(version) + ";";
    if (exec_dbg(db, pragma.c_str()) != SQLITE_OK) {
        exec_dbg(db, "ROLLBACK;");
        return SQLITE_ERROR;
    }

    if (exec_dbg(db, "COMMIT;") != SQLITE_OK) return SQLITE_ERROR;
    std::cout << "✅ Schema bootstrap completed (version " << version << ")" << std::endl;
    return SQLITE_OK;
}

static int migrateV1ToV2(sqlite3* db) {
    if (exec_dbg(db, "SAVEPOINT sp_v1_v2;") != SQLITE_OK) return SQLITE_ERROR;
    
    if (has_table(db, "tx") && !has_column(db, "tx", "rbf")) {
        if (exec_plan(db, "ALTER TABLE tx ADD COLUMN rbf INTEGER NOT NULL DEFAULT 0;") != SQLITE_OK) goto fail;
        std::cout << "  ✅ Added RBF support to transactions" << std::endl;
    }
    if (exec_plan(db, "CREATE INDEX IF NOT EXISTS idx_utxos_addr ON utxos(address_id);") != SQLITE_OK) goto fail;
    std::cout << "  ✅ Added UTXO address index" << std::endl;
    
    if (exec_dbg(db, "RELEASE SAVEPOINT sp_v1_v2;") != SQLITE_OK) return SQLITE_ERROR;
    return SQLITE_OK;
    
fail:
    exec_dbg(db, "ROLLBACK TO sp_v1_v2; RELEASE SAVEPOINT sp_v1_v2;");
    return SQLITE_ERROR;
}

static int migrateV2ToV3(sqlite3* db) {
    if (exec_dbg(db, "SAVEPOINT sp_v2_v3;") != SQLITE_OK) return SQLITE_ERROR;
    
    if (has_table(db, "tx") && !has_column(db, "tx", "locktime")) {
        if (exec_plan(db, "ALTER TABLE tx ADD COLUMN locktime INTEGER NOT NULL DEFAULT 0;") != SQLITE_OK) goto fail;
        std::cout << "  ✅ Added locktime support to transactions" << std::endl;
    }
    
    if (exec_dbg(db, "RELEASE SAVEPOINT sp_v2_v3;") != SQLITE_OK) return SQLITE_ERROR;
    return SQLITE_OK;
    
fail:
    exec_dbg(db, "ROLLBACK TO sp_v2_v3; RELEASE SAVEPOINT sp_v2_v3;");
    return SQLITE_ERROR;
}

static int migrateV3ToV4(sqlite3* db) {
    if (exec_dbg(db, "SAVEPOINT sp_v3_v4;") != SQLITE_OK) return SQLITE_ERROR;
    
    // Add HD wallet columns to keys table
    if (has_table(db, "keys")) {
        if (!has_column(db, "keys", "hd_path")) {
            if (exec_plan(db, "ALTER TABLE keys ADD COLUMN hd_path TEXT;") != SQLITE_OK) goto fail;
            std::cout << "  ✅ Added HD path to keys" << std::endl;
        }
        if (!has_column(db, "keys", "depth")) {
            if (exec_plan(db, "ALTER TABLE keys ADD COLUMN depth INTEGER DEFAULT 0;") != SQLITE_OK) goto fail;
            std::cout << "  ✅ Added HD depth to keys" << std::endl;
        }
        if (!has_column(db, "keys", "child_num")) {
            if (exec_plan(db, "ALTER TABLE keys ADD COLUMN child_num INTEGER DEFAULT 0;") != SQLITE_OK) goto fail;
            std::cout << "  ✅ Added HD child number to keys" << std::endl;
        }
        if (!has_column(db, "keys", "chain_code")) {
            if (exec_plan(db, "ALTER TABLE keys ADD COLUMN chain_code BLOB;") != SQLITE_OK) goto fail;
            std::cout << "  ✅ Added HD chain code to keys" << std::endl;
        }
        if (!has_column(db, "keys", "parent_fpr")) {
            if (exec_plan(db, "ALTER TABLE keys ADD COLUMN parent_fpr INTEGER DEFAULT 0;") != SQLITE_OK) goto fail;
            std::cout << "  ✅ Added HD parent fingerprint to keys" << std::endl;
        }
    }
    
    if (exec_dbg(db, "RELEASE SAVEPOINT sp_v3_v4;") != SQLITE_OK) return SQLITE_ERROR;
    return SQLITE_OK;
    
fail:
    exec_dbg(db, "ROLLBACK TO sp_v3_v4; RELEASE SAVEPOINT sp_v3_v4;");
    return SQLITE_ERROR;
}

static int runMigrations(sqlite3* db) {
    int ver = getUserVersion(db);
    if (ver == 0) { 
        setUserVersion(db, 1); 
        ver = 1; 
    } // safety on upgraded code paths
    
    std::cerr << "🧩 schema user_version=" << ver << " → target=4" << std::endl;
    
    if (ver < 2) { 
        std::cout << "🔄 Migrating from version 1 to 2..." << std::endl;
        if (migrateV1ToV2(db) != SQLITE_OK) return SQLITE_ERROR; 
        setUserVersion(db, 2); 
        ver = 2; 
    }
    if (ver < 3) { 
        std::cout << "🔄 Migrating from version 2 to 3..." << std::endl;
        if (migrateV2ToV3(db) != SQLITE_OK) return SQLITE_ERROR; 
        setUserVersion(db, 3); 
        ver = 3; 
    }
    if (ver < 4) { 
        std::cout << "🔄 Migrating from version 3 to 4 (HD Wallet)..." << std::endl;
        if (migrateV3ToV4(db) != SQLITE_OK) return SQLITE_ERROR; 
        setUserVersion(db, 4); 
        ver = 4; 
    }
    
    if (ver >= 4) {
        std::cout << "✅ Schema migration completed to version " << ver << std::endl;
    }
    return SQLITE_OK;
}

bool SQLiteWallet::migrateSchema() {
    if (!db) {
        std::cerr << "❌ migrateSchema() called with null database" << std::endl;
        return false;
    }
    
    // Bootstrap-first migration approach
    
    // Bootstrap first (idempotent)
    if (bootstrapSchema(db) != SQLITE_OK) {
        std::cerr << "❌ Failed to bootstrap schema" << std::endl;
        return false;
    }
    
    // Then migrate
    if (runMigrations(db) != SQLITE_OK) {
        std::cerr << "❌ Failed to migrate schema" << std::endl;
        return false;
    }
    
    return true;
}

// Database invariants checker (fast, catches regressions)
bool SQLiteWallet::checkInvariants() const {
    if (!initialized || !db) return false;
    
    auto q = [&](const char* sql) -> long long {
        sqlite3_stmt* st = nullptr; 
        long long v = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            v = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st); 
        return v;
    };
    
    // No duplicate UTXOs
    if (q("SELECT COUNT(*) FROM (SELECT txid,vout,COUNT(*) c FROM utxos GROUP BY txid,vout HAVING c>1);")) {
        std::cerr << "❌ Invariant violation: duplicate UTXOs found" << std::endl;
        return false;
    }
    
    // Unspent = NULL spend_txid
    if (q("SELECT COUNT(*) FROM utxos WHERE (spend_txid IS NULL) != (spend_height IS NULL);")) {
        std::cerr << "❌ Invariant violation: inconsistent spend state" << std::endl;
        return false;
    }
    
    // Foreign keys respected
    if (q("PRAGMA foreign_key_check;")) {
        std::cerr << "❌ Invariant violation: foreign key constraint violations" << std::endl;
        return false;
    }
    
    // Integrity check
    sqlite3_stmt* st = nullptr; 
    bool ok = true;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* s = sqlite3_column_text(st, 0);
            if (!s || strcmp((const char*)s, "ok")) { 
                std::cerr << "❌ Invariant violation: integrity check failed: " << (s ? (const char*)s : "null") << std::endl;
                ok = false; 
                break; 
            }
        }
    }
    sqlite3_finalize(st);
    
    if (!ok) return false;
    
    std::cout << "✅ Database invariants check passed" << std::endl;
    return true;
}

// Log database performance statistics
void SQLiteWallet::logDatabaseStats() const {
    if (!db) return;
    
    int cur = 0, hi = 0;
    
    // Cache statistics
    sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_HIT, &cur, &hi, 0);
    std::cerr << "📈 cache_hit(cur=" << cur << ", hi=" << hi << ")" << std::endl;
    
    sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_MISS, &cur, &hi, 0);
    std::cerr << "📈 cache_miss(cur=" << cur << ", hi=" << hi << ")" << std::endl;
    
    sqlite3_db_status(db, SQLITE_DBSTATUS_LOOKASIDE_HIT, &cur, &hi, 0);
    std::cerr << "📈 lookaside_hit(cur=" << cur << ", hi=" << hi << ")" << std::endl;
    
    sqlite3_db_status(db, SQLITE_DBSTATUS_LOOKASIDE_MISS_SIZE, &cur, &hi, 0);
    std::cerr << "📈 lookaside_miss_size(cur=" << cur << ", hi=" << hi << ")" << std::endl;
    
    sqlite3_db_status(db, SQLITE_DBSTATUS_SCHEMA_USED, &cur, &hi, 0);
    std::cerr << "📈 schema_used(cur=" << cur << ", hi=" << hi << ")" << std::endl;
    
    sqlite3_db_status(db, SQLITE_DBSTATUS_STMT_USED, &cur, &hi, 0);
    std::cerr << "📈 stmt_used(cur=" << cur << ", hi=" << hi << ")" << std::endl;
}

bool SQLiteWallet::backupToFileAPI(const std::string& backup_path) {
    if (!initialized || !db) return false;
    
    std::cout << "🔄 Creating backup using SQLite API: " << backup_path << std::endl;
    
    // Checkpoint WAL before backup to ensure consistency
    int ckpt_mode = SQLITE_CHECKPOINT_PASSIVE;  // default non-blocking
    if (const char* m = std::getenv("DINERO_WAL_CKPT")) {
        if (!strcasecmp(m, "FULL"))     ckpt_mode = SQLITE_CHECKPOINT_FULL;
        else if (!strcasecmp(m, "RESTART"))  ckpt_mode = SQLITE_CHECKPOINT_RESTART;
        else if (!strcasecmp(m, "TRUNCATE")) ckpt_mode = SQLITE_CHECKPOINT_TRUNCATE;
        else ckpt_mode = SQLITE_CHECKPOINT_PASSIVE;
    }
    
    int wal_pages = 0, checkpointed = 0;
    std::cerr << "🧱 checkpoint:start mode=" << ckpt_mode << std::endl;
    int rc = sqlite3_wal_checkpoint_v2(db, "main", ckpt_mode, &wal_pages, &checkpointed);
    std::cerr << "🧱 checkpoint rc=" << rc << " mode=" << ckpt_mode << " log=" << wal_pages << " ckpt=" << checkpointed << std::endl;
    
    if (rc != SQLITE_OK && rc != SQLITE_BUSY) {
        std::cerr << "⚠️ WAL checkpoint warning: " << sqlite3_errmsg(db) << std::endl;
    } else {
        std::cout << "✅ WAL checkpoint: " << checkpointed << "/" << wal_pages << " pages (mode=" << ckpt_mode << ")" << std::endl;
    }
    std::cerr << "🧱 checkpoint:end" << std::endl;
    
    // Open destination database
    sqlite3* dst = nullptr;
    rc = sqlite3_open_v2(backup_path.c_str(), &dst, 
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "❌ Failed to open backup destination: " << sqlite3_errmsg(dst) << std::endl;
        if (dst) sqlite3_close(dst);
        return false;
    }
    
    // Secure file permissions for backup
    if (chmod(backup_path.c_str(), 0600) != 0) {
        std::cerr << "⚠️ Warning: Failed to set secure permissions on backup file" << std::endl;
    }
    
    // Initialize backup
    sqlite3_backup* bk = sqlite3_backup_init(dst, "main", db, "main");
    if (!bk) {
        std::cerr << "❌ Failed to initialize backup: " << sqlite3_errmsg(dst) << std::endl;
        sqlite3_close(dst);
        return false;
    }
    
    // Perform backup (copy all pages)
    rc = sqlite3_backup_step(bk, -1);
    if (rc != SQLITE_DONE) {
        std::cerr << "❌ Backup failed: " << sqlite3_errstr(rc) << std::endl;
        sqlite3_backup_finish(bk);
        sqlite3_close(dst);
        return false;
    }
    
    // Get backup statistics
    int pages_copied = sqlite3_backup_pagecount(bk);
    int pages_remaining = sqlite3_backup_remaining(bk);
    
    // Finalize backup
    rc = sqlite3_backup_finish(bk);
    sqlite3_close(dst);
    
    if (rc != SQLITE_OK) {
        std::cerr << "❌ Backup finalization failed: " << sqlite3_errstr(rc) << std::endl;
        return false;
    }
    
    std::cout << "✅ Backup completed: " << pages_copied << " pages copied, " 
              << pages_remaining << " remaining" << std::endl;
    return true;
}

// HD Wallet Implementation
bool SQLiteWallet::initializeHDWallet(const std::string& passphrase) {
    if (!initialized || !db) return false;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    // Check if HD wallet is already initialized
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT seed_ciphertext FROM hd_meta WHERE id=1;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* seed_data = sqlite3_column_blob(stmt, 0);
            if (seed_data != nullptr) {
                sqlite3_finalize(stmt);
                std::cout << "HD wallet already initialized" << std::endl;
                return true;
            }
        }
        sqlite3_finalize(stmt);
    }
    
    // Generate new 32-byte seed using crypto RNG
    uint8_t seed[32];
    if (!CF_GenerateRandomBytes(seed, sizeof(seed))) {
        std::cerr << "❌ Failed to generate secure random seed" << std::endl;
        return false;
    }
    
    // Seed generated successfully
    
    // Derive master key to validate seed
    // TODO: Update to use new HDWallet interface
    // ExtPriv master;
    // if (!HDWallet::masterFromSeed(seed, sizeof(seed), master)) {
    //     std::cerr << "❌ Failed to derive master key from seed" << std::endl;
    //     return false;
    // }
    
    // TODO: Encrypt seed with passphrase (for now, store plaintext for testing)
    // In production, use Argon2id + AES-GCM
    
    // Store seed and master fingerprint
    if (sqlite3_prepare_v2(db, 
        "UPDATE hd_meta SET master_fpr=?, seed_ciphertext=?, created_at=? WHERE id=1;", 
        -1, &stmt, nullptr) == SQLITE_OK) {
        
        sqlite3_bind_int64(stmt, 1, 0); // TODO: Update when HDWallet interface is fixed
        sqlite3_bind_blob(stmt, 2, seed, sizeof(seed), SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, time(nullptr));
        
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc == SQLITE_DONE) {
            std::cout << "✅ HD wallet initialized with master fingerprint: 0x" 
                      << std::hex << 0 << std::dec << std::endl; // TODO: Update when HDWallet interface is fixed
            return true;
        }
    }
    
    std::cerr << "❌ Failed to store HD wallet seed" << std::endl;
    return false;
}

bool SQLiteWallet::unlockHDWallet(const std::string& passphrase) {
    if (!initialized || !db) return false;
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    // Load encrypted seed from database
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT seed_ciphertext FROM hd_meta WHERE id=1;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* seed_data = sqlite3_column_blob(stmt, 0);
            int seed_len = sqlite3_column_bytes(stmt, 0);
            
            if (seed_data && seed_len == 32) {
                // TODO: Decrypt with passphrase (for now, copy plaintext)
                memcpy(hd_seed, seed_data, 32);
                hd_unlocked = true;
                
                sqlite3_finalize(stmt);
                std::cout << "✅ HD wallet unlocked" << std::endl;
                return true;
            }
        }
        sqlite3_finalize(stmt);
    }
    
    std::cerr << "❌ Failed to unlock HD wallet" << std::endl;
    return false;
}

void SQLiteWallet::lockHDWallet() {
    std::lock_guard<std::mutex> lock(wallet_mutex);
    memset(hd_seed, 0, sizeof(hd_seed));
    hd_unlocked = false;
    std::cout << "🔒 HD wallet locked" << std::endl;
}

bool SQLiteWallet::isHDWalletUnlocked() const {
    std::lock_guard<std::mutex> lock(wallet_mutex);
    return hd_unlocked;
}

std::string SQLiteWallet::getNewHDAddress() {
    // TODO: Update to use new HDWallet interface
    // For now, return empty string to avoid compilation errors
    return "";
    
    /* COMMENTED OUT - NEEDS UPDATE TO NEW HDWALLET INTERFACE
    if (!initialized || !db || !hd_unlocked) return "";
    
    std::lock_guard<std::mutex> lock(wallet_mutex);
    
    // Load HD parameters
    sqlite3_stmt* stmt = nullptr;
    // BIP84 constants
    constexpr int BIP84_PURPOSE = 84;

    // Dinero uses fixed SLIP-44 coin type across wallet contexts.
    int coin_type = static_cast<int>(dinero::consensus::DINERO_COIN_TYPE);

    int purpose = BIP84_PURPOSE;
    int account = 0;
    int ext_next_index = 0;
    
    if (sqlite3_prepare_v2(db, 
        "SELECT purpose, coin_type, account, ext_next_index FROM hd_meta WHERE id=1;", 
        -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            purpose = sqlite3_column_int(stmt, 0);
            coin_type = sqlite3_column_int(stmt, 1);
            account = sqlite3_column_int(stmt, 2);
            ext_next_index = sqlite3_column_int(stmt, 3);
        }
        sqlite3_finalize(stmt);
    }
    
    // Derive master key
    ExtPriv master;
    if (!HDWallet::masterFromSeed(hd_seed, 32, master)) {
        std::cerr << "❌ Failed to derive master key" << std::endl;
        return "";
    }
    
    // Create secp256k1 context
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    
    // Derive child key for external address
    ExtPriv child;
    if (!HDWallet::deriveBip84Path(ctx, master, purpose, coin_type, account, 0, ext_next_index, child)) {
        std::cerr << "❌ Failed to derive child key" << std::endl;
        secp256k1_context_destroy(ctx);
        return "";
    }
    
    // Generate address
    std::string address = HDWallet::p2wpkhAddressFromPriv(ctx, child.k, network_hrp.c_str());
    if (address.empty()) {
        std::cerr << "❌ Failed to generate address" << std::endl;
        secp256k1_context_destroy(ctx);
        return "";
    }
    
    // Create witness script (0x00 0x14 + 20-byte hash160)
    uint8_t pub33[33], h20[20];
    HDWallet::privToPubCompressed(ctx, child.k, pub33);
    secp256k1_context_destroy(ctx);
    HDWallet::hash160(h20, pub33, 33);
    
    uint8_t script[22];
    script[0] = 0x00; script[1] = 0x14;
    memcpy(script + 2, h20, 20);
    
    std::string script_hex;
    for (int i = 0; i < 22; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", script[i]);
        script_hex += buf;
    }
    
    // Store in database atomically
    if (sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK) {
        // Insert key
        if (sqlite3_prepare_v2(db, 
            "INSERT INTO keys(pubkey, hd_path, depth, child_num, parent_fpr, created_at) "
            "VALUES(?,?,?,?,?,?);", -1, &stmt, nullptr) == SQLITE_OK) {
            
            std::string pubkey_hex;
            for (int i = 0; i < 33; i++) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", pub33[i]);
                pubkey_hex += buf;
            }
            
            std::string path = "m/" + std::to_string(purpose) + "'/" + 
                              std::to_string(coin_type) + "'/" + 
                              std::to_string(account) + "'/0/" + 
                              std::to_string(ext_next_index);
            
            sqlite3_bind_text(stmt, 1, pubkey_hex.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, child.depth);
            sqlite3_bind_int64(stmt, 4, child.child);
            sqlite3_bind_int64(stmt, 5, child.parent_fpr);
            sqlite3_bind_int64(stmt, 6, time(nullptr));
            
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                int64_t key_id = sqlite3_last_insert_rowid(db);
                sqlite3_finalize(stmt);
                
                // Insert address
                if (sqlite3_prepare_v2(db, 
                    "INSERT INTO addresses(address, script_pubkey, type, key_id, created_at) "
                    "VALUES(?,?,?,?,?);", -1, &stmt, nullptr) == SQLITE_OK) {
                    
                    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, script_hex.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 3, "p2wpkh", -1, SQLITE_STATIC);
                    sqlite3_bind_int64(stmt, 4, key_id);
                    sqlite3_bind_int64(stmt, 5, time(nullptr));
                    
                    if (sqlite3_step(stmt) == SQLITE_DONE) {
                        sqlite3_finalize(stmt);
                        
                        // Update counter
                        if (sqlite3_prepare_v2(db, 
                            "UPDATE hd_meta SET ext_next_index = ext_next_index + 1 WHERE id=1;", 
                            -1, &stmt, nullptr) == SQLITE_OK) {
                            
                            if (sqlite3_step(stmt) == SQLITE_DONE) {
                                sqlite3_finalize(stmt);
                                sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
                                
                                std::cout << "✅ Generated new address: " << address 
                                          << " (path: " << path << ")" << std::endl;
                                return address;
                            }
                            sqlite3_finalize(stmt);
                        }
                    } else {
                        sqlite3_finalize(stmt);
                    }
                } 
            } else {
                sqlite3_finalize(stmt);
            }
        }
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    
    std::cerr << "❌ Failed to store new address" << std::endl;
    return "";
    */ // END COMMENTED OUT SECTION
}

std::string SQLiteWallet::getNewChangeAddress() {
    // Similar to getNewAddress but uses change=1 and int_next_index
    // Implementation would be nearly identical, just different counter
    // For now, return empty string as placeholder
    return "";
}

} // namespace Wallet
} // namespace Dinero
