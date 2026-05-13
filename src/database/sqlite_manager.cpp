#include "database/sqlite_manager.h"
#include "sqlite_open.h"
#include <algorithm>
#include "common/logger.h"
// #include "daemon/db_init_simple.hpp"  // REMOVED: Zombie - never called
#include "daemon/db_meta_utils.hpp"
// #include "consensus/chainparams_simple.hpp"  // REMOVED: Zombie - legacy struct, never used
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <climits>  // PATH_MAX

#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#elif defined(__linux__)
#include <unistd.h>  // readlink
#endif

namespace dinero {

namespace {
bool IsEmbeddedContainerPath(const std::string& path) {
    return path.find("/Library/Containers/") != std::string::npos ||
           path.find("/var/mobile/Containers/") != std::string::npos;
}
}  // namespace

// ============================================================================
// SCHEMA VERSION CONSTANTS (Phase 3 - November 2025)
// ============================================================================
// These define the schema versions supported by this build of dinerod.
// When you add a migration, increment the version and create the migration file.

constexpr int kWalletSchemaVersion = 1;    // Dinero 0.1.0
[[maybe_unused]] constexpr int kExplorerSchemaVersion = 1;  // Dinero 0.1.0
[[maybe_unused]] constexpr int kMempoolSchemaVersion = 1;   // Dinero 0.1.0
[[maybe_unused]] constexpr int kPeersSchemaVersion = 1;     // Dinero 0.1.0

// ============================================================================

SQLiteManager::SQLiteManager(const std::string& datadir) : datadir_(datadir) {
    // Create database paths
    // NOTE: Only wallet.db should use SQLite (Bitcoin Core model)
    // Removed: explorer.db, mempool.db, peers.db (should use RocksDB/binary)
    std::filesystem::create_directories(datadir);

    wallet_db_path_ = datadir + "/wallet.db";
    // REMOVED: blockchain_db_path_ (explorer.db) - queries should go to ChainDB (RocksDB)
    // REMOVED: mempool_db_path_ (mempool.db) - should use RocksDB or in-memory
    // REMOVED: peers_db_path_ (peers.db) - should use binary format
}

SQLiteManager::~SQLiteManager() {
    closeDatabase(wallet_db_);
    // REMOVED: closeDatabase(blockchain_db_) - no longer using explorer.db
    // REMOVED: closeDatabase(mempool_db_) - no longer using mempool.db
    // REMOVED: closeDatabase(peers_db_) - no longer using peers.db
}

bool SQLiteManager::initialize() {
    dinero::g_logger.info("Initializing SQLite databases (wallet only - Bitcoin Core model)...");

    if (!initializeWalletDB()) {
        dinero::g_logger.error("Failed to initialize wallet database");
        return false;
    }

    // REMOVED: initializeBlockchainDB() - no longer using explorer.db
    // REMOVED: initializeMempoolDB() - no longer using mempool.db
    // REMOVED: initializePeersDB() - no longer using peers.db

    dinero::g_logger.info("SQLite wallet database initialized successfully");
    return true;
}

bool SQLiteManager::initializeWalletDB() {
    // CRITICAL FIX: Use unified SQLite opener with consistent PRAGMAs
    auto opened = open_sqlite(wallet_db_path_);
    if (opened.rc != SQLITE_OK) {
        dinero::g_logger.error("Failed to open wallet database: " + opened.errmsg);
        return false;
    }
    wallet_db_ = opened.db;
    
    if (!createWalletSchema()) {
        return false;
    }
    
    // Run migrations if needed (Phase 3)
    try {
        if (!runMigrations(wallet_db_, "wallet", kWalletSchemaVersion)) {
            dinero::g_logger.warning("[Wallet] Migration runner completed with warnings");
        }
    } catch (const std::exception& e) {
        dinero::g_logger.error("[Wallet] Migration failed: " + std::string(e.what()));
        return false;
    }
    
    dinero::g_logger.info("Wallet database initialized: " + wallet_db_path_);
    return true;
}

// ============================================================================
// REMOVED: initializeBlockchainDB(), initializeMempoolDB(), initializePeersDB()
// These are obsolete - blockchain data is in ChainDB (RocksDB), mempool is RAM-only
// ============================================================================
// SQL-FIRST SCHEMA LOADING (Phase 2 - November 2025)
// ============================================================================

std::string SQLiteManager::loadSchemaFile(const std::string& schema_name) {
    namespace fs = std::filesystem;
    
    // Get executable's directory for portable distribution
    std::string exe_dir;
    try {
        // Get executable path (macOS, Linux, Windows compatible)
        #ifdef __APPLE__
            char path[PATH_MAX];
            uint32_t size = sizeof(path);
            if (_NSGetExecutablePath(path, &size) == 0) {
                exe_dir = fs::path(path).parent_path().string();
            }
        #elif defined(__linux__)
            char path[PATH_MAX];
            ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
            if (count != -1) {
                path[count] = '\0';
                exe_dir = fs::path(path).parent_path().string();
            }
        #else
            exe_dir = ".";  // Fallback for unknown platforms
        #endif
    } catch (...) {
        exe_dir = ".";
    }

    const bool embedded_runtime =
        IsEmbeddedContainerPath(datadir_) || IsEmbeddedContainerPath(exe_dir);

    // Try multiple search paths (prioritize portable runtime paths).
    std::vector<std::string> search_paths = {
        exe_dir + "/../resources/schema/" + schema_name,
        exe_dir + "/resources/schema/" + schema_name,
        "./resources/schema/" + schema_name,
        exe_dir + "/../database/schema/" + schema_name,
        exe_dir + "/database/schema/" + schema_name,
        "./database/schema/" + schema_name,
        "/usr/local/share/dinerod/schema/" + schema_name,
    };

    // Dev-only source tree fallbacks. Never prefer these in app container runtime.
    if (!embedded_runtime) {
        search_paths.push_back(std::string(CMAKE_SOURCE_DIR) + "/resources/schema/" + schema_name);
        search_paths.push_back(std::string(CMAKE_SOURCE_DIR) + "/database/schema/" + schema_name);
    }

    for (const auto& path : search_paths) {
        std::error_code ec;
        if (!fs::exists(path, ec) || ec) {
            continue;
        }
        if (!fs::is_regular_file(path, ec) || ec) {
            continue;
        }

        std::ifstream file(path, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
            dinero::g_logger.warning("[SQLiteManager] Schema candidate exists but is not readable: " + path);
            continue;
        }

        dinero::g_logger.info("[SQLiteManager] Loading schema from: " + path);

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string sql = buffer.str();

        if (sql.empty()) {
            dinero::g_logger.warning("[SQLiteManager] Schema file is empty: " + path);
            continue;
        }

        dinero::g_logger.info("[SQLiteManager] ✅ Loaded " + std::to_string(sql.size()) +
                            " bytes from " + schema_name);
        return sql;
    }

    throw std::runtime_error(
        "[SQLiteManager] FATAL: Missing readable schema file: " + schema_name +
        " (searched " + std::to_string(search_paths.size()) + " locations)");
}

int SQLiteManager::getSchemaVersion(sqlite3* db, const std::string& component) {
    const char* sql = "SELECT version FROM schema_version WHERE component = ? LIMIT 1";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        // Table might not exist yet (first run)
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, component.c_str(), -1, SQLITE_TRANSIENT);
    
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return version;
}

bool SQLiteManager::applySchema(sqlite3* db, const std::string& sql, const std::string& component_name) {
    // Execute schema SQL (may contain multiple statements)
    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        dinero::g_logger.error("[SQLiteManager] Schema application failed for " + component_name + 
                             ": " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    
    // Verify schema version was set
    int version = getSchemaVersion(db, component_name);
    if (version > 0) {
        dinero::g_logger.info("[SQLiteManager] ✅ " + component_name + " schema v" + 
                            std::to_string(version) + " applied successfully");
    } else {
        dinero::g_logger.warning("[SQLiteManager] ⚠️  Schema applied but version not found for " + 
                               component_name);
    }
    
    return true;
}

// ============================================================================
// MIGRATION RUNNER (Phase 3 - November 2025)
// ============================================================================

std::vector<std::string> SQLiteManager::scanMigrationFiles(const std::string& component) {
    namespace fs = std::filesystem;
    std::vector<std::string> migration_files;
    
    // Get executable's directory (same logic as loadSchemaFile)
    std::string exe_dir;
    try {
        #ifdef __APPLE__
            char path[PATH_MAX];
            uint32_t size = sizeof(path);
            if (_NSGetExecutablePath(path, &size) == 0) {
                exe_dir = fs::path(path).parent_path().string();
            }
        #elif defined(__linux__)
            char path[PATH_MAX];
            ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
            if (count != -1) {
                path[count] = '\0';
                exe_dir = fs::path(path).parent_path().string();
            }
        #else
            exe_dir = ".";
        #endif
    } catch (...) {
        exe_dir = ".";
    }

    const bool embedded_runtime =
        IsEmbeddedContainerPath(datadir_) || IsEmbeddedContainerPath(exe_dir);

    // Try multiple search paths for migrations directory (portable first).
    std::vector<std::string> search_paths = {
        exe_dir + "/../database/schema/migrations/" + component,
        exe_dir + "/database/schema/migrations/" + component,
        "./database/schema/migrations/" + component,
        "/usr/local/share/dinerod/schema/migrations/" + component,
    };
    if (!embedded_runtime) {
        search_paths.push_back(std::string(CMAKE_SOURCE_DIR) + "/database/schema/migrations/" + component);
    }

    for (const auto& base_path : search_paths) {
        std::error_code ec;
        if (!fs::exists(base_path, ec) || ec || !fs::is_directory(base_path, ec) || ec) {
            continue;
        }

        for (const auto& entry : fs::directory_iterator(base_path, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file(ec) && !ec && entry.path().extension() == ".sql") {
                migration_files.push_back(entry.path().filename().string());
            }
        }

        if (!migration_files.empty()) {
            std::sort(migration_files.begin(), migration_files.end());
            return migration_files;
        }
    }
    
    return migration_files;  // Empty if no migrations found
}

int SQLiteManager::parseMigrationVersion(const std::string& filename) {
    // Parse version from filename format: 002_add_indexes.sql -> 2
    // or 003_add_labels.sql -> 3
    if (filename.size() < 3) return 0;
    
    try {
        std::string version_str = filename.substr(0, 3);
        return std::stoi(version_str);
    } catch (...) {
        return 0;
    }
}

bool SQLiteManager::runMigrations(sqlite3* db, const std::string& component, int target_version) {
    int current_version = getSchemaVersion(db, component);
    
    if (current_version == target_version) {
        dinero::g_logger.info("[SQLiteManager] " + component + " schema v" + 
                            std::to_string(current_version) + " (target v" + 
                            std::to_string(target_version) + ") — OK");
        return true;
    }
    
    if (current_version > target_version) {
        throw std::runtime_error("[SQLiteManager] FATAL: " + component + 
                               " schema v" + std::to_string(current_version) + 
                               " is newer than supported v" + std::to_string(target_version) + 
                               " (database too new for this binary)");
    }
    
    // Need to run migrations: current < target
    dinero::g_logger.info("[SQLiteManager] Upgrading " + component + " schema v" + 
                        std::to_string(current_version) + " → v" + 
                        std::to_string(target_version));
    
    auto migration_files = scanMigrationFiles(component);
    if (migration_files.empty()) {
        dinero::g_logger.warning("[SQLiteManager] No migration files found for " + component);
        return false;
    }
    
    // Apply migrations in order
    for (const auto& migration_file : migration_files) {
        int migration_version = parseMigrationVersion(migration_file);
        
        // Only apply migrations between current and target
        if (migration_version > current_version && migration_version <= target_version) {
            dinero::g_logger.info("[SQLiteManager] Applying migration: " + migration_file);
            
            std::string migration_sql = loadSchemaFile("migrations/" + component + "/" + migration_file);
            
            char* err_msg = nullptr;
            if (sqlite3_exec(db, migration_sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
                dinero::g_logger.error("[SQLiteManager] Migration failed: " + std::string(err_msg));
                sqlite3_free(err_msg);
                return false;
            }
            
            dinero::g_logger.info("[SQLiteManager] ✅ Applied migration " + migration_file);
        }
    }
    
    // Verify final version
    int final_version = getSchemaVersion(db, component);
    if (final_version == target_version) {
        dinero::g_logger.info("[SQLiteManager] ✅ " + component + " schema upgraded to v" + 
                            std::to_string(final_version));
        return true;
    } else {
        dinero::g_logger.error("[SQLiteManager] Migration completed but version mismatch: got v" + 
                             std::to_string(final_version) + ", expected v" + 
                             std::to_string(target_version));
        return false;
    }
}

// ============================================================================
// SCHEMA CREATION (SQL-First Architecture)
// ============================================================================

bool SQLiteManager::createWalletSchema() {
    // SQL-first approach: Try loading from .sql file first
    try {
        std::string schema_sql = loadSchemaFile("wallet_schema.sql");
        if (applySchema(wallet_db_, schema_sql, "wallet")) {
            dinero::g_logger.info("[SQLiteManager] 🎉 Wallet schema loaded from wallet_schema.sql");
            return true;
        }
    } catch (const std::exception& e) {
        dinero::g_logger.warning("[SQLiteManager] Could not load wallet_schema.sql, using embedded schema: " + 
                               std::string(e.what()));
    }
    
    // FALLBACK: Embedded schema (backward compatibility)
    dinero::g_logger.info("[SQLiteManager] Using embedded wallet schema (legacy mode)");
    
    // Bitcoin Core-compatible wallet schema
    const std::vector<std::string> wallet_schema = {
        // Wallet metadata
        "CREATE TABLE IF NOT EXISTS wallet ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT UNIQUE NOT NULL,"
        "  encrypted BOOLEAN DEFAULT 0,"
        "  created_at INTEGER DEFAULT (strftime('%s', 'now'))"
        ")",
        
        // HD wallet master keys
        "CREATE TABLE IF NOT EXISTS hd_master ("
        "  id INTEGER PRIMARY KEY,"
        "  wallet_id INTEGER NOT NULL,"
        "  seed_hash BLOB NOT NULL,"
        "  encrypted_seed BLOB,"
        "  chain_code BLOB NOT NULL,"
        "  fingerprint BLOB NOT NULL,"
        "  depth INTEGER DEFAULT 0,"
        "  child_num INTEGER DEFAULT 0,"
        "  created_at INTEGER DEFAULT (strftime('%s', 'now')),"
        "  FOREIGN KEY(wallet_id) REFERENCES wallet(id)"
        ")",
        
        // HD wallet accounts (BIP44)
        "CREATE TABLE IF NOT EXISTS hd_accounts ("
        "  id INTEGER PRIMARY KEY,"
        "  wallet_id INTEGER NOT NULL,"
        "  account_index INTEGER NOT NULL,"
        "  purpose INTEGER NOT NULL,"  // 44 for BIP44, 84 for BIP84
        "  coin_type INTEGER NOT NULL," // 0 for Bitcoin, 1 for testnet
        "  account_index_hardened INTEGER NOT NULL,"
        "  xpub BLOB NOT NULL,"
        "  created_at INTEGER DEFAULT (strftime('%s', 'now')),"
        "  FOREIGN KEY(wallet_id) REFERENCES wallet(id),"
        "  UNIQUE(wallet_id, account_index)"
        ")",
        
        // Addresses
        "CREATE TABLE IF NOT EXISTS addresses ("
        "  id INTEGER PRIMARY KEY,"
        "  account_id INTEGER NOT NULL,"
        "  address_index INTEGER NOT NULL,"
        "  address TEXT NOT NULL,"
        "  script_pubkey BLOB NOT NULL,"
        "  type TEXT NOT NULL DEFAULT 'p2wpkh' CHECK(type IN('p2wpkh','p2wsh')),"
        "  is_change BOOLEAN DEFAULT 0,"
        "  label TEXT,"
        "  created_at INTEGER DEFAULT (strftime('%s', 'now')),"
        "  FOREIGN KEY(account_id) REFERENCES hd_accounts(id),"
        "  UNIQUE(account_id, address_index, is_change)"
        ")",
        
        // Transactions
        "CREATE TABLE IF NOT EXISTS transactions ("
        "  id INTEGER PRIMARY KEY,"
        "  wallet_id INTEGER NOT NULL,"
        "  tx_hash BLOB UNIQUE NOT NULL,"
        "  block_height INTEGER,"
        "  block_time INTEGER,"
        "  fee INTEGER,"
        "  amount INTEGER,"  // Net amount (positive for incoming, negative for outgoing)
        "  confirmations INTEGER DEFAULT 0,"
        "  created_at INTEGER DEFAULT (strftime('%s', 'now')),"
        "  FOREIGN KEY(wallet_id) REFERENCES wallet(id)"
        ")",
        
        // Transaction inputs/outputs
        "CREATE TABLE IF NOT EXISTS tx_io ("
        "  id INTEGER PRIMARY KEY,"
        "  tx_id INTEGER NOT NULL,"
        "  is_input BOOLEAN NOT NULL,"
        "  address_id INTEGER,"
        "  amount INTEGER NOT NULL,"
        "  script_sig BLOB,"
        "  script_pubkey BLOB NOT NULL,"
        "  sequence INTEGER DEFAULT 0xffffffff,"
        "  witness_data BLOB,"
        "  FOREIGN KEY(tx_id) REFERENCES transactions(id),"
        "  FOREIGN KEY(address_id) REFERENCES addresses(id)"
        ")"
    };
    
    for (const auto& sql : wallet_schema) {
        if (!executeSQL(wallet_db_, sql)) {
            return false;
        }
    }
    
    // Create indexes for performance
    const std::vector<std::string> wallet_indexes = {
        "CREATE INDEX IF NOT EXISTS idx_addresses_account ON addresses(account_id)",
        "CREATE INDEX IF NOT EXISTS idx_addresses_address ON addresses(address)",
        "CREATE INDEX IF NOT EXISTS idx_transactions_hash ON transactions(tx_hash)",
        "CREATE INDEX IF NOT EXISTS idx_transactions_height ON transactions(block_height)",
        "CREATE INDEX IF NOT EXISTS idx_tx_io_tx ON tx_io(tx_id)"
    };
    
    for (const auto& sql : wallet_indexes) {
        if (!executeSQL(wallet_db_, sql)) {
            return false;
        }
    }
    
    return true;
}

// ============================================================================
// REMOVED: createBlockchainSchema(), createMempoolSchema(), createPeersSchema()
// These are obsolete - blockchain data is in ChainDB (RocksDB), mempool is RAM-only
// ============================================================================


bool SQLiteManager::executeSQL(sqlite3* db, const std::string& sql) {
    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        dinero::g_logger.error("SQL execution failed: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

void SQLiteManager::closeDatabase(sqlite3*& db) {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

} // namespace dinero
