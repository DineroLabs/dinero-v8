#include "wallet/wallet_manager.h"
#include "consensus/coin_type.h"
#include "consensus/subsidy.h"   // For ConsensusSubsidy::UNA_PER_DIN
#include "wallet/hd_wallet.h"    // For HDWallet Taproot address methods
#include "wallet/taproot_keys.h" // Canonical TapTweak (ComputeTweakedPubkey)
#include "wallet/retired_coin_type_guard.h"
#include "wallet/shielded_derivation.h"
#include "wallet/shielded_wallet_ops.h"
#include "wallet/utxo_index.h"   // For UTXOIndex address registration
#include "wallet/hd_paths.h"
#include "common/address_script_builder.h" // For BuildScriptPubKeyFromAddress
#include "consensus/chainparams.h"       // For GetActiveChain
#include "wallet/descriptor_store.h"  // Phase 3C: For descriptor-based address generation
// #include "lightning/lightning_service.h"  // DISABLED: Lightning is standalone
#include "storage/archival_block_reader.h"
#include "sqlite_open.h"
#include "sqlite_txn.h"
#include "common/logger.h"
#include "common/ilogger.h"           // Dependency injection interface
#include "common/production_logger.h" // Default logger implementation
#include "db/db_log.h"
#include "crypto/secure_random.h"
#include "crypto/hd_keychain.h"  // For HD wallet key derivation
#include "wallet/bip32_deriver.h"  // For BIP32 derivation (canonical engine)
#include "wallet/bip39.h"
#include "wallet/v7_p2mr_store.h"
#include "crypto/pbkdf2.h"       // For key derivation from passphrase
#include "crypto/wallet_crypto.h"// For AES-256-GCM encryption/decryption
#include <openssl/evp.h>         // For AES-256-GCM encryption
#include <openssl/rand.h>        // For secure random bytes
#include <openssl/crypto.h>      // For OPENSSL_cleanse
#include <openssl/sha.h>         // For SHA256
#include <openssl/ripemd.h>      // For RIPEMD160
#include <secp256k1.h>           // For EC key operations
#include <secp256k1_extrakeys.h> // For x-only/taproot operations
#include <array>
#include <atomic>
#include <cassert>               // For assert() in debug builds
#include "wallet/address.h"
#include "wallet/key_identity.h"  // Week 1 Day 2: KeyID for descriptor wallet
#include "wallet/key_origin.h"    // Week 1 Day 2: KeyOriginInfo for descriptor wallet
#include "address/addr_codec.h"
#include "daemon/mempool.h"
#include "dinero/core/common/AddressCodec.h"  // For P2TR address encoding
#include "external/bech32/bech32.hpp"  // For Bech32/Bech32m encoding
#include "primitives/block.h"    // Phase 3D: For Block structure in wallet notifications
#include "util/hex.h"             // For hex encoding/decoding binary data in settings
#include <cerrno>
#ifndef _WIN32
#include <sys/stat.h> // For stat() and file permissions
#include <unistd.h>
#else
#include <io.h>
#include <direct.h>
#endif

// Forward declare coinbase maturity functions to avoid namespace conflicts
namespace dinero {
    class CoinbaseMaturity {
    public:
        static constexpr uint32_t COINBASE_MATURITY = 100;
        static bool isCoinbaseMature(uint32_t coinbase_height, uint32_t current_height) {
            if (current_height < coinbase_height) return false;
            uint32_t confirmations = current_height - coinbase_height + 1;
            return confirmations >= COINBASE_MATURITY;
        }
        static uint32_t getBlocksUntilMature(uint32_t coinbase_height, uint32_t current_height) {
            if (isCoinbaseMature(coinbase_height, current_height)) return 0;
            if (current_height < coinbase_height) return COINBASE_MATURITY;
            uint32_t confirmations = current_height - coinbase_height + 1;
            return COINBASE_MATURITY - confirmations;
        }
    };
}
#include <sqlite3.h>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>                    // For rescanBlockchain watch_scripts matching
#include "storage/chain_db.h"     // For rescanBlockchain ChainDB access
#include "crypto/dinero_crypto_minimal.h"

namespace dinero {

// ═══════════════════════════════════════════════════════════════
// Logging Macros - Dependency Injection Pattern
// ═══════════════════════════════════════════════════════════════
#define WLOG_DEBUG(msg) do { if (logger_) logger_->debug(msg); } while(0)
#define WLOG_INFO(msg)  do { if (logger_) logger_->info(msg); } while(0)
#define WLOG_WARN(msg)  do { if (logger_) logger_->warning(msg); } while(0)
#define WLOG_ERR(msg)   do { if (logger_) logger_->error(msg); } while(0)

// Helper function for consistent spendability logic
inline bool IsSpendable(bool is_coinbase, int confs, int minconf = 1) {
    return (!is_coinbase && confs >= minconf) || (is_coinbase && confs >= 100);
}

// ═══════════════════════════════════════════════════════════════
// Database Corruption Detection (Seatbelt)
// Log once per category, skip bad rows, continue operation
// Companion to --repair-db (the tow truck)
// ═══════════════════════════════════════════════════════════════
namespace {
    static std::atomic<int> g_corrupt_rows_skipped{0};
    static std::atomic<bool> g_repair_suggested{false};

    void logCorruptRow(const char* table, const char* column, const char* issue) {
        int count = ++g_corrupt_rows_skipped;

        // Log first few, then summarize
        if (count <= 3) {
            g_logger.warning("[DB-SEATBELT] Skipping corrupt row in " +
                           std::string(table) + "." + std::string(column) +
                           ": " + std::string(issue));
        }

        // Suggest --repair-db once after multiple issues
        if (count == 5 && !g_repair_suggested.exchange(true)) {
            g_logger.warning("[DB-SEATBELT] Multiple corrupt rows detected. "
                           "Run 'dinerod --repair-db' to scan and fix database issues.");
        }
    }
}

#ifndef _WIN32
namespace {

class ScopedUmask {
public:
    explicit ScopedUmask(mode_t new_mask) : old_mask_(::umask(new_mask)) {}
    ~ScopedUmask() { ::umask(old_mask_); }

private:
    mode_t old_mask_;
};

std::string ModeToOctal(mode_t mode) {
    std::ostringstream oss;
    oss << "0" << std::oct << (mode & 0777);
    return oss.str();
}

bool EnsurePathPermissions(const std::filesystem::path& path,
                           mode_t expected_mode,
                           bool* adjusted,
                           std::string* error_msg) {
    if (adjusted) {
        *adjusted = false;
    }

    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (error_msg) {
            *error_msg = "stat(" + path.string() + ") failed: " + std::string(std::strerror(errno));
        }
        return false;
    }

    mode_t current_mode = st.st_mode & 0777;
    if (current_mode == expected_mode) {
        return true;
    }

    if (::chmod(path.c_str(), expected_mode) != 0) {
        if (error_msg) {
            *error_msg = "chmod(" + path.string() + ", " + ModeToOctal(expected_mode) +
                         ") failed: " + std::string(std::strerror(errno));
        }
        return false;
    }

    if (::stat(path.c_str(), &st) != 0) {
        if (error_msg) {
            *error_msg = "post-chmod stat(" + path.string() + ") failed: " + std::string(std::strerror(errno));
        }
        return false;
    }

    mode_t verified_mode = st.st_mode & 0777;
    if (verified_mode != expected_mode) {
        if (error_msg) {
            *error_msg = "permission verification failed for " + path.string() +
                         ": expected " + ModeToOctal(expected_mode) +
                         ", got " + ModeToOctal(verified_mode);
        }
        return false;
    }

    if (adjusted) {
        *adjusted = true;
    }
    return true;
}

}  // namespace
#endif // !_WIN32

static void secureClearBytes(std::vector<uint8_t>& data) {
    if (!data.empty()) {
        OPENSSL_cleanse(data.data(), data.size());
        data.clear();
    }
}

static void secureClearString(std::string& data) {
    if (!data.empty()) {
        OPENSSL_cleanse(data.data(), data.size());
        data.clear();
    }
}

// Encryption flow must never reset derivation/UTXO tables.
static constexpr bool kResetAddressStateDuringEncryption = false;
static_assert(!kResetAddressStateDuringEncryption,
              "Wallet encryption must not reset address/derivation state");

namespace {

constexpr char kBip39RecoverySetting[] = "bip39_recovery_v1";
constexpr char kBip39BackupAcknowledgedSetting[] = "bip39_backup_acknowledged";
constexpr uint8_t kBip39RecoveryRecordVersion = 1;
constexpr uint8_t kBip39PassphraseRequired = 1u << 0;
constexpr size_t kBip39RecoveryNonceSize = 12;
constexpr char kBip39RecoveryKeyDomain[] = "Dinero WalletManager BIP39 recovery v1";

std::array<uint8_t, 32> DeriveBip39RecoveryKey(const std::vector<uint8_t>& seed) {
    std::vector<uint8_t> material;
    material.reserve(sizeof(kBip39RecoveryKeyDomain) - 1 + seed.size());
    material.insert(material.end(),
                    kBip39RecoveryKeyDomain,
                    kBip39RecoveryKeyDomain + sizeof(kBip39RecoveryKeyDomain) - 1);
    material.insert(material.end(), seed.begin(), seed.end());

    std::array<uint8_t, 32> key{};
    ::sha256(material.data(), material.size(), key.data());
    OPENSSL_cleanse(material.data(), material.size());
    return key;
}

bool ConstantTimeEqual(const std::vector<uint8_t>& lhs,
                       const std::vector<uint8_t>& rhs) {
    return lhs.size() == rhs.size() &&
           CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

void SetRecoveryError(std::string* error_out, const std::string& message) {
    if (error_out) {
        *error_out = message;
    }
}

}  // namespace

// Helper function to convert bytes to hex string
static std::string bytesToHex(const uint8_t* bytes, size_t size) {
    static const char* hex_chars = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        hex += hex_chars[(bytes[i] >> 4) & 0xF];
        hex += hex_chars[bytes[i] & 0xF];
    }
    return hex;
}

// Compute BIP341 output key for key-path spend (no script tree)
// Thin adapter — delegates to canonical TaprootKeys::ComputeTweakedPubkey
static bool ComputeTaprootOutputKey(const std::vector<uint8_t>& internal_xonly,
                                    std::array<uint8_t, 32>& output_key,
                                    ILogger* logger) {
    if (internal_xonly.size() != 32) {
        if (logger) logger->error("Taproot internal key must be 32 bytes");
        return false;
    }
    std::array<uint8_t, 32> internal_arr;
    std::copy(internal_xonly.begin(), internal_xonly.end(), internal_arr.begin());
    return dinero::TaprootKeys::ComputeTweakedPubkey(internal_arr, output_key);
}

WalletManager::WalletManager(const std::filesystem::path& dataDir,
                             ILogger* logger,
                             const std::string& walletSchemaPath)
    : dataDir_(dataDir),
      current_wallet_id_(-1),
      logger_(logger ? logger : &ProductionLogger::instance()),
      wallet_schema_path_override_(walletSchemaPath) {
    {
        // Create wallet directories with restrictive defaults.
#ifndef _WIN32
        ScopedUmask restrictive_umask(0077);
#endif
        std::filesystem::create_directories(dataDir_);
        std::filesystem::create_directories(dataDir_ / "wallets");
    }

#ifndef _WIN32
    bool adjusted = false;
    std::string perm_error;
    if (!EnsurePathPermissions(dataDir_, 0700, &adjusted, &perm_error)) {
        throw std::runtime_error("Failed to secure wallet data directory: " + perm_error);
    }
    if (adjusted) {
        WLOG_WARN("Auto-corrected wallet data directory permissions to 0700: " + dataDir_.string());
    }

    adjusted = false;
    if (!EnsurePathPermissions(dataDir_ / "wallets", 0700, &adjusted, &perm_error)) {
        throw std::runtime_error("Failed to secure wallet database directory: " + perm_error);
    }
    if (adjusted) {
        WLOG_WARN("Auto-corrected wallet database directory permissions to 0700: " +
                  (dataDir_ / "wallets").string());
    }
#endif

    // Initialize wallet registry (lightweight, always open)
    initializeRegistry();

    // Note: Individual wallet databases are opened via open(wallet_name)
    // db_ remains nullptr until a wallet is selected
}

WalletManager::~WalletManager() {
    close();
    closeRegistry();
}

std::string WalletManager::resolveWalletSchemaPath() const {
    auto isEmbeddedContainerPath = [](const std::filesystem::path& path) {
        const std::string s = path.string();
        return s.find("/Library/Containers/") != std::string::npos ||
               s.find("/var/mobile/Containers/") != std::string::npos;
    };

    std::vector<std::filesystem::path> candidates;
    candidates.reserve(8);

    if (!wallet_schema_path_override_.empty()) {
        candidates.emplace_back(wallet_schema_path_override_);
    }

    candidates.emplace_back(dataDir_ / "schema" / "wallet_schema.sql");
    candidates.emplace_back(dataDir_ / "wallet_schema.sql");

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        candidates.emplace_back(cwd / "resources" / "schema" / "wallet_schema.sql");
        candidates.emplace_back(cwd / "wallet_schema.sql");
    }

    // Source-tree fallback for developer builds.
    // Never use this in embedded app-container runtime.
    if (!isEmbeddedContainerPath(dataDir_)) {
        candidates.emplace_back(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                                "resources" / "schema" / "wallet_schema.sql");
    }

    for (const auto& candidate : candidates) {
        std::error_code stat_ec;
        if (candidate.empty()) {
            continue;
        }
        if (!std::filesystem::exists(candidate, stat_ec) || stat_ec) {
            continue;
        }
        if (!std::filesystem::is_regular_file(candidate, stat_ec) || stat_ec) {
            continue;
        }

        std::ifstream probe(candidate.string());
        if (probe.is_open()) {
            return candidate.string();
        }
    }

    return "";
}

void WalletManager::initializeRegistry() {
    const auto registryPath = (dataDir_ / "wallet_registry.db").string();

    WLOG_INFO("WalletManager opening registry: " + registryPath);

    // Use unified SQLite opener
    auto opened = open_sqlite(registryPath);
    if (opened.rc != SQLITE_OK) {
        throw std::runtime_error("Cannot open wallet registry: " + opened.errmsg);
    }
    registry_db_ = opened.db;

    WLOG_INFO("Wallet registry opened successfully");

    // Enable WAL mode and foreign keys
    exec(registry_db_, "PRAGMA foreign_keys = ON");
    exec(registry_db_, "PRAGMA journal_mode = WAL");
    exec(registry_db_, "PRAGMA trusted_schema = OFF");

    // Create registry tables if they don't exist
    // Schema: wallets (id, name, path, network, encrypted, fingerprint, created_at, last_opened)
    exec(registry_db_, R"(
        CREATE TABLE IF NOT EXISTS wallets (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            path TEXT NOT NULL UNIQUE,
            network TEXT NOT NULL DEFAULT 'mainnet',
            encrypted INTEGER NOT NULL DEFAULT 0,
            fingerprint BLOB,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            last_opened INTEGER
        )
    )");

    exec(registry_db_, "CREATE INDEX IF NOT EXISTS idx_registry_name ON wallets(name)");
    exec(registry_db_, "CREATE INDEX IF NOT EXISTS idx_registry_encrypted ON wallets(encrypted)");
    exec(registry_db_, "CREATE INDEX IF NOT EXISTS idx_registry_last_opened ON wallets(last_opened)");

    exec(registry_db_, R"(
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER PRIMARY KEY,
            applied_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        )
    )");

    exec(registry_db_, "INSERT OR IGNORE INTO schema_version (version) VALUES (1)");

    WLOG_INFO("Wallet registry initialized");
}

void WalletManager::initializeDatabase() {
    // This method is now called per-wallet when open() is invoked
    // It opens the specific wallet_<name>.db file
    if (!db_) {
        throw std::runtime_error("initializeDatabase() called but db_ is nullptr - use open(wallet_name) instead");
    }

    WLOG_INFO("Initializing wallet database");

    // Enable foreign keys and WAL mode
    exec(db_, "PRAGMA foreign_keys = ON");
    exec(db_, "PRAGMA journal_mode = WAL");
    exec(db_, "PRAGMA trusted_schema = OFF");

    // Run schema migrations (upgrades database to current version)
    migrate(db_);
    ensureWalletIdentityRow();

    // Run health check
    runHealthCheck();

    // Check file permissions
    checkFilePermissions();

    WLOG_INFO("Wallet database initialized");
}

void WalletManager::ensureWalletIdentityRow() {
    if (!db_) {
        return;
    }
    if (!tableExists(db_, "wallets")) {
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* exists_sql = "SELECT 1 FROM wallets WHERE id = 1 LIMIT 1";
    if (sqlite3_prepare_v2(db_, exists_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_ROW) {
            return;
        }
    } else {
        if (stmt) sqlite3_finalize(stmt);
        WLOG_WARN("Failed to probe wallets(id=1): " + std::string(sqlite3_errmsg(db_)));
    }

    std::string wallet_name = current_;
    if (wallet_name.empty() && tableExists(db_, "wallet_meta")) {
        const char* name_sql = "SELECT name FROM wallet_meta WHERE id = 1 LIMIT 1";
        if (sqlite3_prepare_v2(db_, name_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (text && text[0] != '\0') {
                    wallet_name = text;
                }
            }
            sqlite3_finalize(stmt);
        } else if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
    if (wallet_name.empty()) {
        wallet_name = "default";
    }

    auto ensure_inserted = [&](const std::string& candidate_name) -> bool {
        sqlite3_stmt* insert_stmt = nullptr;
        const char* insert_sql = "INSERT OR IGNORE INTO wallets (id, name) VALUES (1, ?)";
        if (sqlite3_prepare_v2(db_, insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
            if (insert_stmt) sqlite3_finalize(insert_stmt);
            return false;
        }
        sqlite3_bind_text(insert_stmt, 1, candidate_name.c_str(), -1, SQLITE_TRANSIENT);
        const int step_rc = sqlite3_step(insert_stmt);
        sqlite3_finalize(insert_stmt);
        if (step_rc != SQLITE_DONE) {
            return false;
        }

        sqlite3_stmt* verify_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, exists_sql, -1, &verify_stmt, nullptr) != SQLITE_OK) {
            if (verify_stmt) sqlite3_finalize(verify_stmt);
            return false;
        }
        const bool found = (sqlite3_step(verify_stmt) == SQLITE_ROW);
        sqlite3_finalize(verify_stmt);
        return found;
    };

    if (ensure_inserted(wallet_name)) {
        WLOG_WARN("Inserted missing wallets(id=1) identity row for compatibility");
        return;
    }

    if (ensure_inserted(wallet_name + "_id1")) {
        WLOG_WARN("Inserted missing wallets(id=1) identity row using fallback name");
        return;
    }

    WLOG_ERR("Failed to create wallets(id=1) identity row; address inserts may fail with FK errors");
}

void WalletManager::migrate(sqlite3* db) {
    int version = getUserVersion(db);
    
    if (version < 1) {
        // Create wallets table
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS wallets (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL UNIQUE,
                seed_fingerprint BLOB,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");
        
        // Create addresses table with wallet_id FK
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS addresses (
                id INTEGER PRIMARY KEY,
                wallet_id INTEGER NOT NULL REFERENCES wallets(id) ON DELETE CASCADE,
                account INTEGER NOT NULL DEFAULT 0,
                change INTEGER NOT NULL DEFAULT 0,
                idx INTEGER NOT NULL,
                address TEXT NOT NULL,
                pubkey BLOB,
                label TEXT,
                type TEXT NOT NULL DEFAULT 'p2wpkh' CHECK(type IN('p2wpkh','p2wsh','p2tr')),
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(wallet_id, account, change, idx),
                UNIQUE(address)
            )
        )");
        
        // Create indexes
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_wallet ON addresses(wallet_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_path ON addresses(wallet_id, account, change, idx)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_bech32 ON addresses(address)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_label ON addresses(wallet_id, label)");
        
        setUserVersion(db, 1);
    }
    
    if (version < 2) {
        // Create wallet_addresses table for external address book entries
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS wallet_addresses (
                id INTEGER PRIMARY KEY,
                wallet_id INTEGER NOT NULL REFERENCES wallets(id) ON DELETE CASCADE,
                address TEXT NOT NULL,
                label TEXT,
                account INTEGER,
                change INTEGER,
                idx INTEGER,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(wallet_id, address)
            )
        )");
        
        // Create indexes
        exec(db, "CREATE INDEX IF NOT EXISTS idx_wallet_addr_wallet ON wallet_addresses(wallet_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_wallet_addr_address ON wallet_addresses(address)");
        
        setUserVersion(db, 2);
    }
    
    if (version < 3) {
        // Create watch_scripts table for chainstate sync
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS watch_scripts (
                script_pubkey BLOB PRIMARY KEY,
                path TEXT,
                is_change INTEGER NOT NULL DEFAULT 0,
                last_seen_height INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");
        
        // Create sync_meta table for rescan tracking
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS sync_meta (
                id INTEGER PRIMARY KEY CHECK (id=1),
                last_scanned_height INTEGER NOT NULL DEFAULT 0,
                birth_height INTEGER NOT NULL DEFAULT 0,
                gap_limit INTEGER NOT NULL DEFAULT 20,
                updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");

        // Initialize sync_meta with birth_height=0 (updated by RPC on wallet creation)
        exec(db, "INSERT OR IGNORE INTO sync_meta (id, birth_height, gap_limit) VALUES (1, 0, 20)");

        // Create indexes for watch_scripts
        exec(db, "CREATE INDEX IF NOT EXISTS idx_watch_scripts_path ON watch_scripts(path)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_watch_scripts_change ON watch_scripts(is_change)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_watch_scripts_height ON watch_scripts(last_seen_height)");

        setUserVersion(db, 3);
    }
    
    if (version < 4) {
        // Create transactions table for wallet transaction history
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS transactions (
                id INTEGER PRIMARY KEY,
                wallet_id INTEGER NOT NULL REFERENCES wallets(id) ON DELETE CASCADE,
                txid TEXT NOT NULL,
                address TEXT NOT NULL,
                amount REAL NOT NULL,
                confirmations INTEGER NOT NULL DEFAULT 0,
                category TEXT NOT NULL,
                label TEXT,
                time INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                is_coinbase INTEGER NOT NULL DEFAULT 0,
                UNIQUE(wallet_id, txid, address)
            )
        )");
        
        // Create indexes for transaction queries
        exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_wallet_id ON transactions(wallet_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_address ON transactions(address)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_category ON transactions(category)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_time ON transactions(time DESC)");
        
        setUserVersion(db, 4);
        WLOG_INFO("Database migrated to schema version 4 with transactions table");
    }
    
    if (version < 5) {
        // Create UTXOs table for precise accounting
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS utxos (
                id INTEGER PRIMARY KEY,
                wallet_id INTEGER NOT NULL REFERENCES wallets(id) ON DELETE CASCADE,
                txid TEXT NOT NULL,
                vout INTEGER NOT NULL,
                address TEXT NOT NULL,
                amount INTEGER NOT NULL,
                script_pubkey TEXT NOT NULL,
                height INTEGER NOT NULL,
                is_coinbase INTEGER NOT NULL DEFAULT 0,
                is_mature INTEGER NOT NULL DEFAULT 0,
                is_spent INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(wallet_id, txid, vout)
            )
        )");
        
        // Create indexes for UTXO queries
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_wallet_id ON utxos(wallet_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_spent ON utxos(is_spent)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_mature ON utxos(is_mature)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_coinbase ON utxos(is_coinbase)");
        
        // Create tip table to track blockchain height
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS tip (
                rowid INTEGER PRIMARY KEY CHECK (rowid = 1),
                height INTEGER NOT NULL DEFAULT 0
            )
        )");
        
            // Initialize tip table if empty
            exec(db, "INSERT OR IGNORE INTO tip (rowid, height) VALUES (1, 0)");

            setUserVersion(db, 5);
            WLOG_INFO("Database migrated to schema version 5 with UTXOs and tip tables");
    }
    
    // Future migrations would go here
    if (version < 6) {
        // Add indices for wallet queries
        exec(db, R"(
            CREATE INDEX IF NOT EXISTS idx_utxos_wallet_spent_height
            ON utxos(wallet_id, is_spent, height)
        )");

        exec(db, R"(
            CREATE INDEX IF NOT EXISTS idx_utxos_wallet_spent_coinbase_height
            ON utxos(wallet_id, is_spent, is_coinbase, height)
        )");

        exec(db, R"(
            CREATE INDEX IF NOT EXISTS idx_utxos_wallet_spent_coinbase
            ON utxos(wallet_id, is_spent, is_coinbase)
        )");

        // Run ANALYZE to update query planner statistics
        exec(db, "ANALYZE");

        setUserVersion(db, 6);
        WLOG_INFO("Database migrated to schema version 6 with UTXOs, tip tables, indices, and ANALYZE");
    }

    if (version < 7) {
        // ═══════════════════════════════════════════════════════════════
        // Phase 1: HD Wallet Private Key Derivation - Database Schema
        // ═══════════════════════════════════════════════════════════════

        // Create hd_seeds table for encrypted master seed storage
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS hd_seeds (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                encrypted_seed BLOB NOT NULL,
                salt BLOB NOT NULL,
                coin_type INTEGER NOT NULL DEFAULT 1448,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");

        // Create address_derivation_paths table for tracking HD derivation
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS address_derivation_paths (
                address TEXT PRIMARY KEY,
                derivation_path TEXT NOT NULL,
                script_pubkey TEXT,
                account INTEGER NOT NULL DEFAULT 0,
                change INTEGER NOT NULL DEFAULT 0,
                address_index INTEGER NOT NULL,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");

        // Create indexes for HD wallet queries
        exec(db, "CREATE INDEX IF NOT EXISTS idx_derivation_path ON address_derivation_paths(derivation_path)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_address_derivation_paths_script_pubkey ON address_derivation_paths(script_pubkey)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_derivation_account_change ON address_derivation_paths(account, change, address_index)");

        setUserVersion(db, 7);
        WLOG_INFO("Database migrated to schema version 7 with HD wallet tables (hd_seeds, address_derivation_paths)");
    }

    if (version < 8) {
        // ═══════════════════════════════════════════════════════════════
        // Phase 2: Taproot Support - Add 'p2tr' to addresses table CHECK constraint
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 8: Adding Taproot (p2tr) address support");

        // SQLite doesn't support ALTER TABLE for CHECK constraints, so we need to recreate the table
        exec(db, "PRAGMA foreign_keys=off");

        // Backup existing data
        exec(db, "CREATE TEMPORARY TABLE addresses_backup AS SELECT * FROM addresses");

        // Drop old table
        exec(db, "DROP TABLE addresses");

        // Recreate with updated CHECK constraint
        exec(db, R"(
            CREATE TABLE addresses (
                id INTEGER PRIMARY KEY,
                wallet_id INTEGER NOT NULL REFERENCES wallets(id) ON DELETE CASCADE,
                account INTEGER NOT NULL DEFAULT 0,
                change INTEGER NOT NULL DEFAULT 0,
                idx INTEGER NOT NULL,
                address TEXT NOT NULL,
                pubkey BLOB,
                label TEXT,
                type TEXT NOT NULL DEFAULT 'p2wpkh' CHECK(type IN('p2wpkh','p2wsh','p2tr')),
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(wallet_id, account, change, idx),
                UNIQUE(address)
            )
        )");

        // Restore data
        exec(db, "INSERT INTO addresses SELECT * FROM addresses_backup");
        exec(db, "DROP TABLE addresses_backup");

        // Recreate indexes
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_wallet ON addresses(wallet_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_path ON addresses(wallet_id, account, change, idx)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_bech32 ON addresses(address)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_label ON addresses(wallet_id, label)");

        exec(db, "PRAGMA foreign_keys=on");

        setUserVersion(db, 8);
        WLOG_INFO("Database migrated to schema version 8 with Taproot (p2tr) address support");
    }

    if (version < 9) {
        // ═══════════════════════════════════════════════════════════════
        // Phase: Wallet Security - Encryption metadata tracking
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 9: Adding encryption metadata table");

        // Create encryption_metadata table for tracking wallet encryption state
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS encryption_metadata (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                encrypted INTEGER NOT NULL DEFAULT 0,          -- 1 if encrypted, 0 if not
                kdf TEXT NOT NULL DEFAULT 'argon2id',           -- Key derivation function
                kdf_iterations INTEGER,                         -- Argon2id time cost (iterations)
                kdf_memory_kb INTEGER,                          -- Argon2id memory cost in KB
                kdf_parallelism INTEGER,                        -- Argon2id parallelism factor
                cipher TEXT NOT NULL DEFAULT 'AES-256-GCM',     -- Encryption cipher
                salt BLOB,                                      -- Argon2id salt (16 bytes for encrypted wallets)
                nonce BLOB,                                     -- AES-GCM nonce/IV (12 bytes for encrypted wallets)
                master_fingerprint BLOB,                        -- BIP32 master key fingerprint (4 bytes, hex: 8 chars)
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");

        // Create index for encryption metadata queries
        exec(db, "CREATE INDEX IF NOT EXISTS idx_encryption_encrypted ON encryption_metadata(encrypted)");

        // Update hd_seeds table to store encrypted seed with tag
        // The encrypted_seed column now stores: salt + nonce + ciphertext + tag
        // For unencrypted wallets, it stores the raw seed
        exec(db, R"(
            ALTER TABLE hd_seeds
            ADD COLUMN encryption_version INTEGER NOT NULL DEFAULT 1
        )");

        setUserVersion(db, 9);
        WLOG_INFO("Database migrated to schema version 9 with encryption metadata tracking");
    }

    if (version < 10) {
        // ═══════════════════════════════════════════════════════════════
        // Week 1 Day 2: Descriptor Wallet Foundation - KeyID tracking
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 10: Adding KeyID and wallet_id columns for descriptor wallet");

        // Add wallet_id column for per-wallet database architecture (always 1)
        if (!columnExists(db, "addresses", "wallet_id")) {
            exec(db, "ALTER TABLE addresses ADD COLUMN wallet_id INTEGER NOT NULL DEFAULT 1");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_wallet ON addresses(wallet_id)");
        }

        // Add KeyID columns to addresses table
        // - key_id: primary identifier (HASH160 of pubkey)
        // - internal_key_id: for Taproot internal key (before TapTweak)
        // - output_key_id: for Taproot output key (after TapTweak)
        //
        // For P2WPKH: only key_id is populated
        // For Taproot: all three are populated (key_id == internal_key_id)

        exec(db, "ALTER TABLE addresses ADD COLUMN key_id BLOB");
        exec(db, "ALTER TABLE addresses ADD COLUMN internal_key_id BLOB");
        exec(db, "ALTER TABLE addresses ADD COLUMN output_key_id BLOB");

        // Add indexes for KeyID lookups (critical for IsMine queries)
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_key_id ON addresses(key_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_addr_output_key_id ON addresses(output_key_id)");

        setUserVersion(db, 10);
        WLOG_INFO("Database migrated to schema version 10 with wallet_id and KeyID columns");
    }

    // Version 11: Add script_pubkey column for Bitcoin-grade ownership checks
    if (version < 11) {
        WLOG_INFO("Migrating database to version 11: Adding script_pubkey column");

        if (!columnExists(db, "addresses", "script_pubkey")) {
            exec(db, "ALTER TABLE addresses ADD COLUMN script_pubkey TEXT");
            WLOG_INFO("Added script_pubkey column to addresses table");
        }

        setUserVersion(db, 11);
        WLOG_INFO("Database migrated to schema version 11 with script_pubkey column");
    }

    // Version 12: Add script_pubkey to address_derivation_paths for scriptPubKey-based key lookups
    if (version < 12) {
        WLOG_INFO("Migrating database to version 12: Adding script_pubkey to address_derivation_paths");

        if (!columnExists(db, "address_derivation_paths", "script_pubkey")) {
            exec(db, "ALTER TABLE address_derivation_paths ADD COLUMN script_pubkey TEXT");
            WLOG_INFO("Added script_pubkey column to address_derivation_paths table");

            // Backfill script_pubkey from addresses table where possible
            exec(db, R"(
                UPDATE address_derivation_paths
                SET script_pubkey = (
                    SELECT script_pubkey
                    FROM addresses
                    WHERE addresses.address = address_derivation_paths.address
                )
                WHERE EXISTS (
                    SELECT 1
                    FROM addresses
                    WHERE addresses.address = address_derivation_paths.address
                )
            )");
            WLOG_INFO("Backfilled script_pubkey values from addresses table");

            // Create index for fast scriptPubKey-based lookups
            exec(db, "CREATE INDEX IF NOT EXISTS idx_address_derivation_paths_script_pubkey ON address_derivation_paths(script_pubkey)");
            WLOG_INFO("Created index on script_pubkey column");
        }

        setUserVersion(db, 12);
        WLOG_INFO("Database migrated to schema version 12 with scriptPubKey-based key lookups");
    }

    // Version 13: Fix transactions table schema for wallet history
    // (Phase 35.1.1 - Wallet Transaction Ingestion)
    if (version < 13) {
        WLOG_INFO("Migrating database to version 13: Fixing transactions table schema");

        // Add missing columns for transaction history
        if (!columnExists(db, "transactions", "address")) {
            exec(db, "ALTER TABLE transactions ADD COLUMN address TEXT");
            WLOG_INFO("Added address column to transactions table");
        }

        if (!columnExists(db, "transactions", "amount")) {
            exec(db, "ALTER TABLE transactions ADD COLUMN amount REAL NOT NULL DEFAULT 0");
            WLOG_INFO("Added amount column to transactions table");
        }

        if (!columnExists(db, "transactions", "category")) {
            exec(db, "ALTER TABLE transactions ADD COLUMN category TEXT NOT NULL DEFAULT 'unknown'");
            WLOG_INFO("Added category column to transactions table");
        }

        if (!columnExists(db, "transactions", "label")) {
            exec(db, "ALTER TABLE transactions ADD COLUMN label TEXT");
            WLOG_INFO("Added label column to transactions table");
        }

        setUserVersion(db, 13);
        WLOG_INFO("Database migrated to schema version 13 with complete transactions schema");
    }

    // Version 14: Add height column for transaction history reorg handling
    // (Phase 36 - Transaction History Reorg Handling)
    if (version < 14) {
        WLOG_INFO("Migrating database to version 14: Adding height column to transactions table");

        // Add height column to track which block the transaction was confirmed in
        if (!columnExists(db, "transactions", "height")) {
            exec(db, "ALTER TABLE transactions ADD COLUMN height INTEGER NOT NULL DEFAULT 0");
            WLOG_INFO("Added height column to transactions table");

            // Create index for efficient reorg queries (delete by height)
            exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_height ON transactions(height)");
            WLOG_INFO("Created index on transactions.height");
        }

        setUserVersion(db, 14);
        WLOG_INFO("Database migrated to schema version 14 with reorg support");
    }

    if (version < 15) {
        // ═══════════════════════════════════════════════════════════════
        // Add imported_keys table for importprivkey support
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 15: Adding imported_keys table");

        // Create imported_keys table for manually imported private keys
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS imported_keys (
                address TEXT PRIMARY KEY,
                private_key_enc TEXT NOT NULL,
                label TEXT DEFAULT '',
                created_at TEXT NOT NULL DEFAULT (datetime('now'))
            )
        )");

        // Create index for efficient lookups
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_keys_address ON imported_keys(address)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_keys_created ON imported_keys(created_at)");

        setUserVersion(db, 15);
        WLOG_INFO("Database migrated to schema version 15 with imported_keys table");
    }

    if (version < 16) {
        // ═══════════════════════════════════════════════════════════════
        // BIP86 Taproot Default: Add wallet_policy column to wallet_meta
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 16: Adding wallet_policy column");

        // Check if wallet_meta table exists
        sqlite3_stmt* check_stmt = nullptr;
        const char* check_sql = "SELECT name FROM sqlite_master WHERE type='table' AND name='wallet_meta'";
        if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK) {
            int rc = sqlite3_step(check_stmt);
            sqlite3_finalize(check_stmt);

            if (rc == SQLITE_ROW) {
                // wallet_meta table exists, check if wallet_policy column already exists
                sqlite3_stmt* col_check = nullptr;
                const char* col_sql = "SELECT COUNT(*) FROM pragma_table_info('wallet_meta') WHERE name='wallet_policy'";
                bool column_exists = false;

                if (sqlite3_prepare_v2(db, col_sql, -1, &col_check, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(col_check) == SQLITE_ROW) {
                        column_exists = (sqlite3_column_int(col_check, 0) > 0);
                    }
                    sqlite3_finalize(col_check);
                }

                if (!column_exists) {
                    // Add wallet_policy column
                    // Default to 'bip84' for existing wallets (safety - don't change existing behavior)
                    exec(db, "ALTER TABLE wallet_meta ADD COLUMN wallet_policy TEXT NOT NULL DEFAULT 'bip84'");
                    WLOG_INFO("Added wallet_policy column to wallet_meta (defaulting to 'bip84' for existing wallets)");
                } else {
                    WLOG_INFO("wallet_policy column already exists - skipping migration");
                }
            } else {
                WLOG_INFO("wallet_meta table does not exist yet - skipping wallet_policy migration");
            }
        }

        setUserVersion(db, 16);
        WLOG_INFO("Database migrated to schema version 16 with wallet_policy column");
    }

    if (version < 17) {
        // ═══════════════════════════════════════════════════════════════
        // Imported Descriptors: Add watch-only descriptor support
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 17: Adding imported_descriptors tables");

        // Create imported_descriptors table
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS imported_descriptors (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                descriptor TEXT NOT NULL UNIQUE,
                descriptor_type TEXT NOT NULL,
                internal INTEGER NOT NULL DEFAULT 0,
                active INTEGER NOT NULL DEFAULT 0,
                range_start INTEGER NOT NULL DEFAULT 0,
                range_end INTEGER NOT NULL DEFAULT 1000,
                next_index INTEGER NOT NULL DEFAULT 0,
                timestamp INTEGER,
                label TEXT,
                fingerprint TEXT,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");

        // Create indexes for imported_descriptors
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_descriptors_active ON imported_descriptors(active)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_descriptors_internal ON imported_descriptors(internal)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_descriptors_fingerprint ON imported_descriptors(fingerprint)");

        // Create imported_descriptor_addresses table
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS imported_descriptor_addresses (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                descriptor_id INTEGER NOT NULL REFERENCES imported_descriptors(id) ON DELETE CASCADE,
                address_index INTEGER NOT NULL,
                address TEXT NOT NULL UNIQUE,
                script_pubkey BLOB NOT NULL,
                key_id BLOB,
                internal_key_id BLOB,
                output_key_id BLOB,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(descriptor_id, address_index)
            )
        )");

        // Create indexes for imported_descriptor_addresses
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_addresses_descriptor ON imported_descriptor_addresses(descriptor_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_addresses_address ON imported_descriptor_addresses(address)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_addresses_script ON imported_descriptor_addresses(script_pubkey)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_addresses_key_id ON imported_descriptor_addresses(key_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_imported_addresses_output_key_id ON imported_descriptor_addresses(output_key_id)");

        setUserVersion(db, 17);
        WLOG_INFO("Database migrated to schema version 17 with imported_descriptors tables");
    }

    if (version < 18) {
        // ═══════════════════════════════════════════════════════════════
        // Phase 2: Active Descriptor Support
        // Add signing_capability and metadata fields for descriptor activation
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 18: Adding active descriptor support");

        // Add signing_capability column (none/internal/external)
        exec(db, R"(
            ALTER TABLE imported_descriptors
            ADD COLUMN signing_capability TEXT NOT NULL DEFAULT 'none'
            CHECK(signing_capability IN ('none', 'internal', 'external'))
        )");

        // Add key origin fingerprint for policy validation
        exec(db, R"(
            ALTER TABLE imported_descriptors
            ADD COLUMN key_origin_fingerprint TEXT
        )");

        // Add derivation path prefix for BIP compliance check
        exec(db, R"(
            ALTER TABLE imported_descriptors
            ADD COLUMN derivation_path_prefix TEXT
        )");

        // Add activation timestamp for audit trail
        exec(db, R"(
            ALTER TABLE imported_descriptors
            ADD COLUMN activation_timestamp INTEGER
        )");

        // Add activated_by for accountability
        exec(db, R"(
            ALTER TABLE imported_descriptors
            ADD COLUMN activated_by TEXT
        )");

        setUserVersion(db, 18);
        WLOG_INFO("Database migrated to schema version 18 with active descriptor support");
    }

    if (version < 19) {
        // ═══════════════════════════════════════════════════════════════
        // Phase: Address Labels - System vs User Label Distinction
        // Add is_system_label column to track auto-generated labels (mining)
        // ═══════════════════════════════════════════════════════════════

        WLOG_INFO("Migrating database to version 19: Adding is_system_label column");

        // Check if column already exists before adding
        bool column_exists = false;
        sqlite3_stmt* check_stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(addresses)", -1, &check_stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(check_stmt) == SQLITE_ROW) {
                const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(check_stmt, 1));
                if (col_name && std::string(col_name) == "is_system_label") {
                    column_exists = true;
                    break;
                }
            }
            sqlite3_finalize(check_stmt);
        }

        if (!column_exists) {
            // Add is_system_label column (1 = auto-generated like "Coinbase block #123", 0 = user-set)
            exec(db, R"(
                ALTER TABLE addresses
                ADD COLUMN is_system_label INTEGER NOT NULL DEFAULT 0
            )");
            WLOG_INFO("Added is_system_label column to addresses table");
        } else {
            WLOG_INFO("Column is_system_label already exists, skipping");
        }

        setUserVersion(db, 19);
        WLOG_INFO("Database migrated to schema version 19 with system label tracking");
    }

    if (version < 20) {
        // Add birthday_height to wallet_meta for rescan optimization
        WLOG_INFO("Migrating database to version 20: Adding birthday_height to wallet_meta");

        bool has_column = false;
        sqlite3_stmt* pragma_stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(wallet_meta)", -1, &pragma_stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
                const char* col = reinterpret_cast<const char*>(sqlite3_column_text(pragma_stmt, 1));
                if (col && std::string(col) == "birthday_height") {
                    has_column = true;
                    break;
                }
            }
            sqlite3_finalize(pragma_stmt);
        }

        if (!has_column) {
            exec(db, "ALTER TABLE wallet_meta ADD COLUMN birthday_height INTEGER");
            WLOG_INFO("Added birthday_height column to wallet_meta");
        }

        setUserVersion(db, 20);
        WLOG_INFO("Database migrated to schema version 20 with birthday_height");
    }

    if (version < 21) {
        // Normalize legacy mixed schema to per-wallet single-row semantics.
        // Older migrations created hd_seeds/encryption_metadata keyed by wallet_id.
        WLOG_INFO("Migrating database to version 21: Normalizing HD seed/encryption tables");

        if (tableExists(db, "hd_seeds") && !columnExists(db, "hd_seeds", "id")) {
            exec(db, "ALTER TABLE hd_seeds RENAME TO hd_seeds_legacy");
            exec(db, R"(
                CREATE TABLE IF NOT EXISTS hd_seeds (
                    id INTEGER PRIMARY KEY CHECK (id = 1),
                    encrypted_seed BLOB NOT NULL,
                    salt BLOB NOT NULL,
                    coin_type INTEGER NOT NULL DEFAULT 1448,
                    encryption_version INTEGER NOT NULL DEFAULT 1,
                    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
                )
            )");

            const bool legacy_has_encryption_version = columnExists(db, "hd_seeds_legacy", "encryption_version");
            const bool legacy_has_wallet_id = columnExists(db, "hd_seeds_legacy", "wallet_id");
            std::string copy_hd_sql = legacy_has_encryption_version
                ? "INSERT OR REPLACE INTO hd_seeds (id, encrypted_seed, salt, coin_type, encryption_version, created_at) "
                  "SELECT 1, encrypted_seed, salt, COALESCE(coin_type, 1448), COALESCE(encryption_version, 1), "
                  "COALESCE(created_at, strftime('%s','now')) FROM hd_seeds_legacy"
                : "INSERT OR REPLACE INTO hd_seeds (id, encrypted_seed, salt, coin_type, encryption_version, created_at) "
                  "SELECT 1, encrypted_seed, salt, COALESCE(coin_type, 1448), 1, "
                  "COALESCE(created_at, strftime('%s','now')) FROM hd_seeds_legacy";
            if (legacy_has_wallet_id) {
                copy_hd_sql += " ORDER BY CASE WHEN wallet_id = 1 THEN 0 ELSE 1 END, wallet_id";
            }
            copy_hd_sql += " LIMIT 1";
            exec(db, copy_hd_sql.c_str());
            exec(db, "DROP TABLE hd_seeds_legacy");
            WLOG_INFO("Normalized hd_seeds to id=1 schema");
        }

        if (tableExists(db, "encryption_metadata") && !columnExists(db, "encryption_metadata", "id")) {
            exec(db, "ALTER TABLE encryption_metadata RENAME TO encryption_metadata_legacy");
            exec(db, R"(
                CREATE TABLE IF NOT EXISTS encryption_metadata (
                    id INTEGER PRIMARY KEY CHECK (id = 1),
                    encrypted INTEGER NOT NULL DEFAULT 0,
                    kdf TEXT NOT NULL DEFAULT 'argon2id',
                    kdf_iterations INTEGER,
                    kdf_memory_kb INTEGER,
                    kdf_parallelism INTEGER,
                    cipher TEXT NOT NULL DEFAULT 'AES-256-GCM',
                    salt BLOB,
                    nonce BLOB,
                    master_fingerprint BLOB,
                    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
                )
            )");

            const bool legacy_has_wallet_id = columnExists(db, "encryption_metadata_legacy", "wallet_id");
            std::string copy_meta_sql =
                "INSERT OR REPLACE INTO encryption_metadata "
                "(id, encrypted, kdf, kdf_iterations, kdf_memory_kb, kdf_parallelism, cipher, salt, nonce, "
                "master_fingerprint, created_at, updated_at) "
                "SELECT 1, COALESCE(encrypted, 0), COALESCE(kdf, 'argon2id'), kdf_iterations, kdf_memory_kb, "
                "kdf_parallelism, COALESCE(cipher, 'AES-256-GCM'), salt, nonce, master_fingerprint, "
                "COALESCE(created_at, strftime('%s','now')), COALESCE(updated_at, strftime('%s','now')) "
                "FROM encryption_metadata_legacy";
            if (legacy_has_wallet_id) {
                copy_meta_sql += " ORDER BY CASE WHEN wallet_id = 1 THEN 0 ELSE 1 END, wallet_id";
            }
            copy_meta_sql += " LIMIT 1";
            exec(db, copy_meta_sql.c_str());
            exec(db, "DROP TABLE encryption_metadata_legacy");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_encryption_encrypted ON encryption_metadata(encrypted)");
            WLOG_INFO("Normalized encryption_metadata to id=1 schema");
        }

        setUserVersion(db, 21);
        WLOG_INFO("Database migrated to schema version 21 with normalized wallet seed/encryption schema");
    }

    if (version < 22) {
        // ── settings table ──────────────────────────────────────────────
        // Needed by setSetting()/getSetting()/setMiningAddress().
        // wallet_schema.sql creates it, but the inline-fallback path
        // (used on iOS where the .sql file is not bundled) never did.
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");

        // ── sync_meta table + scan_complete column ──────────────────────
        // wallet_schema.sql omits sync_meta entirely; the v3 migration
        // creates it but without scan_complete.  Handle both cases.
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS sync_meta (
                id INTEGER PRIMARY KEY CHECK (id=1),
                last_scanned_height INTEGER NOT NULL DEFAULT 0,
                birth_height INTEGER NOT NULL DEFAULT 0,
                gap_limit INTEGER NOT NULL DEFAULT 20,
                scan_complete INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");
        exec(db, "INSERT OR IGNORE INTO sync_meta (id, birth_height, gap_limit) VALUES (1, 0, 20)");

        // If sync_meta already existed (from v3) but lacks scan_complete
        if (!columnExists(db, "sync_meta", "scan_complete")) {
            exec(db, "ALTER TABLE sync_meta ADD COLUMN scan_complete INTEGER NOT NULL DEFAULT 0");
        }

        // ── utxos.spent_txid / spent_height ─────────────────────────────
        // rescanBlockchain() marks UTXOs spent with txid + height for
        // reorg rollback.  The v5 migration that created utxos omitted them.
        if (!columnExists(db, "utxos", "spent_txid")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN spent_txid TEXT");
        }
        if (!columnExists(db, "utxos", "spent_height")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN spent_height INTEGER");
        }

        // ── utxos.confirmations ─────────────────────────────────────────
        // addUTXO comment documents this column; wallet_schema.sql has it.
        if (!columnExists(db, "utxos", "confirmations")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN confirmations INTEGER DEFAULT 0");
        }

        // ── Extra indexes that wallet_schema.sql provides ───────────────
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_spent_height ON utxos(is_spent, height)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_address ON utxos(address)");

        setUserVersion(db, 22);
        WLOG_INFO("Database migrated to schema version 22 with settings table, sync_meta.scan_complete, utxos spend-tracking columns");
    }

    if (version < 23) {
        // Normalize transactions table for per-wallet history:
        // - guarantee wallet_id column exists
        // - allow send/receive rows to coexist for same txid
        // - keep schema compatible with addTransaction()/listtransactions
        WLOG_INFO("Migrating database to version 23: Normalizing transactions table");

        const bool had_transactions = tableExists(db, "transactions");

        exec(db, "BEGIN IMMEDIATE");
        try {
            exec(db, R"(
                CREATE TABLE IF NOT EXISTS transactions_v23 (
                    id INTEGER PRIMARY KEY,
                    wallet_id INTEGER NOT NULL DEFAULT 1,
                    txid TEXT NOT NULL,
                    address TEXT NOT NULL DEFAULT '',
                    amount REAL NOT NULL DEFAULT 0,
                    confirmations INTEGER NOT NULL DEFAULT 0,
                    category TEXT NOT NULL DEFAULT 'unknown',
                    label TEXT,
                    time INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                    is_coinbase INTEGER NOT NULL DEFAULT 0,
                    height INTEGER NOT NULL DEFAULT 0,
                    UNIQUE(wallet_id, txid, address, category)
                )
            )");

            if (had_transactions) {
                const bool has_txid = columnExists(db, "transactions", "txid");
                if (has_txid) {
                    const bool has_wallet_id = columnExists(db, "transactions", "wallet_id");
                    const bool has_address = columnExists(db, "transactions", "address");
                    const bool has_amount = columnExists(db, "transactions", "amount");
                    const bool has_confirmations = columnExists(db, "transactions", "confirmations");
                    const bool has_category = columnExists(db, "transactions", "category");
                    const bool has_label = columnExists(db, "transactions", "label");
                    const bool has_time = columnExists(db, "transactions", "time");
                    const bool has_is_coinbase = columnExists(db, "transactions", "is_coinbase");
                    const bool has_height = columnExists(db, "transactions", "height");
                    const bool has_created_at = columnExists(db, "transactions", "created_at");

                    const std::string wallet_expr = has_wallet_id ? "COALESCE(wallet_id, 1)" : "1";
                    const std::string address_expr = has_address ? "COALESCE(address, '')" : "''";
                    const std::string amount_expr = has_amount ? "COALESCE(amount, 0)" : "0";
                    const std::string height_expr = has_height ? "COALESCE(height, 0)" : "0";
                    const std::string conf_fallback = "(CASE WHEN " + height_expr + " > 0 THEN 1 ELSE 0 END)";
                    const std::string conf_expr = has_confirmations
                        ? "COALESCE(confirmations, " + conf_fallback + ")"
                        : conf_fallback;
                    const std::string category_expr = has_category ? "COALESCE(category, 'unknown')" : "'unknown'";
                    const std::string label_expr = has_label ? "label" : "NULL";
                    const std::string time_expr = has_time
                        ? "COALESCE(time, strftime('%s','now'))"
                        : (has_created_at ? "COALESCE(created_at, strftime('%s','now'))" : "strftime('%s','now')");
                    const std::string coinbase_expr = has_is_coinbase ? "COALESCE(is_coinbase, 0)" : "0";

                    std::string copy_sql =
                        "INSERT OR IGNORE INTO transactions_v23 "
                        "(wallet_id, txid, address, amount, confirmations, category, label, time, is_coinbase, height) "
                        "SELECT " +
                        wallet_expr + ", txid, " + address_expr + ", " + amount_expr + ", " + conf_expr + ", " +
                        category_expr + ", " + label_expr + ", " + time_expr + ", " + coinbase_expr + ", " +
                        height_expr + " FROM transactions";
                    exec(db, copy_sql.c_str());
                } else {
                    WLOG_WARN("v23 migration: transactions table missing txid column, recreating history table empty");
                }

                exec(db, "DROP TABLE transactions");
            }

            exec(db, "ALTER TABLE transactions_v23 RENAME TO transactions");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_wallet_id ON transactions(wallet_id)");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_address ON transactions(address)");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_category ON transactions(category)");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_time ON transactions(time DESC)");
            exec(db, "CREATE INDEX IF NOT EXISTS idx_transactions_height ON transactions(height)");

            exec(db, "COMMIT");
        } catch (...) {
            exec(db, "ROLLBACK");
            throw;
        }

        setUserVersion(db, 23);
        WLOG_INFO("Database migrated to schema version 23 with normalized transactions table");
    }

    if (version < 24) {
        // ── Harden utxos schema ──────────────────────────────────────────
        // The v5 migration uses CREATE TABLE IF NOT EXISTS, so if
        // another code path (e.g. reference/database.cpp) already created
        // the utxos table with a simpler schema, wallet_id and other
        // columns required by getBalance() are silently missing.
        // This migration guarantees every required column exists.
        WLOG_INFO("Migrating database to version 24: Hardening utxos schema");

        if (!columnExists(db, "utxos", "wallet_id")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN wallet_id INTEGER NOT NULL DEFAULT 1");
            WLOG_WARN("v24: Added missing wallet_id column to utxos table");
        }
        if (!columnExists(db, "utxos", "address")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN address TEXT NOT NULL DEFAULT ''");
        }
        if (!columnExists(db, "utxos", "is_spent")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN is_spent INTEGER NOT NULL DEFAULT 0");
        }
        if (!columnExists(db, "utxos", "is_mature")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN is_mature INTEGER NOT NULL DEFAULT 0");
        }
        if (!columnExists(db, "utxos", "is_coinbase")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN is_coinbase INTEGER NOT NULL DEFAULT 0");
        }
        if (!columnExists(db, "utxos", "created_at")) {
            exec(db, "ALTER TABLE utxos ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0");
        }

        // Ensure indexes exist (safe with IF NOT EXISTS)
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_wallet_id ON utxos(wallet_id)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_spent ON utxos(is_spent)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_utxos_wallet_spent_height ON utxos(wallet_id, is_spent, height)");

        setUserVersion(db, 24);
        WLOG_INFO("Database migrated to schema version 24: utxos schema hardened");
    }

    if (version < 25) {
        // v7: legacy purpose-77 subtree typo cleanup is a no-op on fresh
        // wallets, kept only to bump the schema version on any pre-v7 wallet DB
        // a user might import. Original migration removed; coin type changed to 1448.
        setUserVersion(db, 25);
        WLOG_INFO("Database migrated to schema version 25 (no-op for v7 fresh wallets)");
    }

    if (version < 26) {
        // Profile-v1 covenant recovery records contain public construction
        // data only: checksummed descriptor, derived scriptPubKey, lineage,
        // and an operator label. The script is also registered in
        // watch_scripts by storeCovenantDescriptor().
        exec(db, R"(
            CREATE TABLE IF NOT EXISTS covenant_descriptors (
                descriptor_id TEXT PRIMARY KEY,
                profile TEXT NOT NULL CHECK(profile IN ('ctv', 'ccv')),
                descriptor TEXT NOT NULL UNIQUE,
                script_pubkey BLOB NOT NULL,
                label TEXT NOT NULL DEFAULT '',
                parent_descriptor_id TEXT,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");
        exec(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_covenant_descriptors_script ON covenant_descriptors(script_pubkey)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_covenant_descriptors_profile ON covenant_descriptors(profile)");
        exec(db, "CREATE INDEX IF NOT EXISTS idx_covenant_descriptors_parent ON covenant_descriptors(parent_descriptor_id)");
        setUserVersion(db, 26);
        WLOG_INFO("Database migrated to schema version 26 with covenant recovery descriptors");
    }

    if (version < 27) {
        // Expand the closed profile discriminator without weakening any of
        // the descriptor/script uniqueness guarantees introduced in v26.
        exec(db, R"(
            CREATE TABLE covenant_descriptors_v27 (
                descriptor_id TEXT PRIMARY KEY,
                profile TEXT NOT NULL CHECK(profile IN ('ctv', 'ccv', 'vault')),
                descriptor TEXT NOT NULL UNIQUE,
                script_pubkey BLOB NOT NULL,
                label TEXT NOT NULL DEFAULT '',
                parent_descriptor_id TEXT,
                created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
            )
        )");
        exec(db, R"(
            INSERT INTO covenant_descriptors_v27
            SELECT descriptor_id, profile, descriptor, script_pubkey, label,
                   parent_descriptor_id, created_at
            FROM covenant_descriptors
        )");
        exec(db, "DROP TABLE covenant_descriptors");
        exec(db, "ALTER TABLE covenant_descriptors_v27 RENAME TO covenant_descriptors");
        exec(db, "CREATE UNIQUE INDEX idx_covenant_descriptors_script ON covenant_descriptors(script_pubkey)");
        exec(db, "CREATE INDEX idx_covenant_descriptors_profile ON covenant_descriptors(profile)");
        exec(db, "CREATE INDEX idx_covenant_descriptors_parent ON covenant_descriptors(parent_descriptor_id)");
        setUserVersion(db, 27);
        WLOG_INFO("Database migrated to schema version 27 with personal vault descriptors");
    }
}

std::vector<std::string> WalletManager::listWallets() const {
    std::vector<std::string> wallets;

    if (!registry_db_) {
        WLOG_ERR("Registry database not open");
        return wallets;
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT name FROM wallets ORDER BY name";

    int rc = sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare wallet list query: " + std::string(sqlite3_errmsg(registry_db_)));
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        // SEATBELT: Validate wallet name before use
        if (!name || strlen(name) == 0) {
            logCorruptRow("wallets", "name", "NULL or empty wallet name");
            continue;  // Skip this row, continue with others
        }
        wallets.emplace_back(name);
    }

    sqlite3_finalize(stmt);
    return wallets;
}

std::string WalletManager::getMostRecentlyOpenedWallet() const {
    if (!registry_db_) {
        WLOG_ERR("Registry database not open");
        return "";
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT name FROM wallets "
        "WHERE last_opened IS NOT NULL "
        "ORDER BY last_opened DESC, name ASC "
        "LIMIT 1";

    if (sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        WLOG_ERR("Failed to prepare last-opened wallet query");
        return "";
    }

    std::string wallet_name;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (name && strlen(name) > 0) {
            wallet_name = name;
        } else {
            logCorruptRow("wallets", "name", "NULL or empty wallet name in last_opened query");
        }
    }

    sqlite3_finalize(stmt);
    return wallet_name;
}

bool WalletManager::exists(const std::string& name) const {
    if (!registry_db_) {
        WLOG_ERR("Registry database not open");
        return false;
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT 1 FROM wallets WHERE name = ? LIMIT 1";

    int rc = sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);

    sqlite3_finalize(stmt);
    return found;
}

void WalletManager::create(const std::string& name) {
    std::vector<uint8_t> seed(64);
    if (!CF_GenerateRandomBytes(seed.data(), seed.size())) {
        throw std::runtime_error("Failed to generate initial HD wallet seed");
    }
    try {
        createWithInitialSeed(name, seed, nullptr, "");
    } catch (...) {
        secureClearBytes(seed);
        throw;
    }
    secureClearBytes(seed);
}

void WalletManager::createFromBip39(const std::string& name,
                                    const std::string& mnemonic,
                                    const std::string& bip39_passphrase) {
    if (!bip39::ValidateMnemonic(mnemonic)) {
        throw std::invalid_argument("Invalid BIP39 mnemonic");
    }
    std::vector<uint8_t> seed;
    if (!bip39::MnemonicToSeed(mnemonic, bip39_passphrase, seed) || seed.size() != 64) {
        secureClearBytes(seed);
        throw std::runtime_error("Failed to derive BIP39 wallet seed");
    }
    try {
        createWithInitialSeed(name, seed, &mnemonic, bip39_passphrase);
    } catch (...) {
        secureClearBytes(seed);
        throw;
    }
    secureClearBytes(seed);
}

void WalletManager::createWithInitialSeed(
    const std::string& name,
    const std::vector<uint8_t>& initial_master_seed,
    const std::string* authoritative_mnemonic,
    const std::string& bip39_passphrase) {
    if (initial_master_seed.size() != 64) {
        throw std::invalid_argument("Initial wallet seed must be exactly 64 bytes");
    }
    const std::string cleanName = sanitize(name);
    if (cleanName.empty()) {
        throw std::invalid_argument("Invalid wallet name");
    }

    WLOG_INFO("[CREATE] Creating per-wallet database for: " + cleanName);

    if (exists(cleanName)) {
        throw std::runtime_error("Wallet already exists: " + cleanName);
    }

    // ═══════════════════════════════════════════════════════════════
    // Per-Wallet DB Architecture: Create wallet_<name>.db file
    // ═══════════════════════════════════════════════════════════════

    // Build wallet database path
    std::filesystem::path walletPath = dataDir_ / "wallets" / ("wallet_" + cleanName + ".db");
    std::string walletPathStr = walletPath.string();

    WLOG_INFO("[CREATE] Wallet database path: " + walletPathStr);

    // Check if wallet file already exists (shouldn't happen due to exists() check)
    if (std::filesystem::exists(walletPath)) {
        throw std::runtime_error("Wallet database file already exists: " + walletPathStr);
    }

    // Create new wallet database
    sqlite3* new_wallet_db = nullptr;
    {
#ifndef _WIN32
        // Ensure newly created wallet files default to owner-only permissions.
        ScopedUmask restrictive_umask(0077);
#endif
        auto opened = open_sqlite(walletPathStr);
        if (opened.rc != SQLITE_OK) {
            throw std::runtime_error("Cannot create wallet database: " + opened.errmsg);
        }
        new_wallet_db = opened.db;
    }

    WLOG_INFO("[CREATE] Wallet database file created");

#ifndef _WIN32
    bool adjusted = false;
    std::string perm_error;
    if (!EnsurePathPermissions(walletPath.parent_path(), 0700, &adjusted, &perm_error)) {
        sqlite3_close(new_wallet_db);
        throw std::runtime_error("Failed to secure wallet directory: " + perm_error);
    }
    if (adjusted) {
        WLOG_WARN("[CREATE] Auto-corrected wallet directory permissions to 0700: " +
                  walletPath.parent_path().string());
    }

    adjusted = false;
    if (!EnsurePathPermissions(walletPath, 0600, &adjusted, &perm_error)) {
        sqlite3_close(new_wallet_db);
        throw std::runtime_error("Failed to secure wallet database file: " + perm_error);
    }
    if (adjusted) {
        WLOG_WARN("[CREATE] Auto-corrected wallet DB file permissions to 0600: " + walletPathStr);
    }
#endif

    try {
        auto applyInlineSchemaFallback = [&]() {
            WLOG_WARN("[CREATE] Using inline schema fallback (migrations will expand to latest)");

            // Enable foreign keys and WAL mode
            exec(new_wallet_db, "PRAGMA foreign_keys = ON");
            exec(new_wallet_db, "PRAGMA journal_mode = WAL");
            exec(new_wallet_db, "PRAGMA trusted_schema = OFF");

            // Create essential tables (minimal schema for now - full schema in wallet_schema.sql)
            exec(new_wallet_db, R"(
                CREATE TABLE IF NOT EXISTS wallet_meta (
                    id INTEGER PRIMARY KEY CHECK (id = 1),
                    name TEXT NOT NULL,
                    network TEXT NOT NULL DEFAULT 'mainnet',
                    encrypted INTEGER NOT NULL DEFAULT 0,
                    fingerprint BLOB,
                    wallet_policy TEXT NOT NULL DEFAULT 'bip86',
                    birthday_height INTEGER,
                    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                    version INTEGER NOT NULL DEFAULT 1
                )
            )");

            // Insert wallet metadata
            sqlite3_stmt* meta_stmt = nullptr;
            const char* meta_sql = "INSERT INTO wallet_meta (id, name, network, wallet_policy) VALUES (1, ?, 'mainnet', 'bip86')";
            if (sqlite3_prepare_v2(new_wallet_db, meta_sql, -1, &meta_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(meta_stmt, 1, cleanName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(meta_stmt);
                sqlite3_finalize(meta_stmt);
            }
        };

        const std::string schema_path = resolveWalletSchemaPath();
        if (schema_path.empty()) {
            WLOG_WARN("[CREATE] No wallet schema file found in runtime search paths");
            applyInlineSchemaFallback();
        } else {
            WLOG_INFO("[CREATE] Applying schema from: " + schema_path);
            std::ifstream schemaFile(schema_path);
            if (!schemaFile.is_open()) {
                WLOG_WARN("[CREATE] Cannot open wallet schema file: " + schema_path);
                applyInlineSchemaFallback();
            } else {
                std::stringstream buffer;
                buffer << schemaFile.rdbuf();
                std::string schemaSql = buffer.str();
                schemaFile.close();

                // Execute the schema SQL
                char* errMsg = nullptr;
                if (sqlite3_exec(new_wallet_db, schemaSql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
                    std::string error = errMsg ? errMsg : "Unknown error";
                    sqlite3_free(errMsg);
                    throw std::runtime_error("Failed to apply wallet schema: " + error);
                }
                WLOG_INFO("[CREATE] Schema applied successfully");

                // Set schema version to 11 (wallet_schema.sql contains latest schema)
                setUserVersion(new_wallet_db, 11);

                // Insert wallet metadata
                sqlite3_stmt* meta_stmt = nullptr;
                const char* meta_sql = "INSERT OR REPLACE INTO wallet_meta (id, name, network) VALUES (1, ?, 'mainnet')";
                if (sqlite3_prepare_v2(new_wallet_db, meta_sql, -1, &meta_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(meta_stmt, 1, cleanName.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(meta_stmt);
                    sqlite3_finalize(meta_stmt);
                }
            }
        }

        // Run schema migrations to upgrade to latest version (only if needed)
        migrate(new_wallet_db);

        // Checkpoint WAL to ensure all changes are persisted to disk
        exec(new_wallet_db, "PRAGMA wal_checkpoint(FULL)");

        sqlite3* previous_db = db_;
        std::string previous_current = current_;
        int previous_wallet_id = current_wallet_id_;
        bool previous_encrypted = wallet_encrypted_;
        bool previous_locked = wallet_locked_;
        std::vector<uint8_t> previous_master_seed = master_seed_;

        try {
            db_ = new_wallet_db;
            current_ = cleanName;
            current_wallet_id_ = 1;  // Per-wallet DB always has id=1
            wallet_encrypted_ = false;
            wallet_locked_ = false;
            ensureWalletIdentityRow();

            if (!storeMasterSeed(initial_master_seed, "", false)) {
                throw std::runtime_error("Failed to persist initial HD wallet seed");
            }
            if (authoritative_mnemonic) {
                std::string recovery_error;
                if (!storeAuthoritativeBip39Mnemonic(
                        *authoritative_mnemonic, bip39_passphrase, &recovery_error)) {
                    throw std::runtime_error(
                        "Failed to persist initial BIP39 recovery material: " + recovery_error);
                }
            }
        } catch (...) {
            db_ = previous_db;
            current_ = previous_current;
            current_wallet_id_ = previous_wallet_id;
            wallet_encrypted_ = previous_encrypted;
            wallet_locked_ = previous_locked;
            master_seed_ = previous_master_seed;
            secureClearBytes(previous_master_seed);
            throw;
        }

        db_ = previous_db;
        current_ = previous_current;
        current_wallet_id_ = previous_wallet_id;
        wallet_encrypted_ = previous_encrypted;
        wallet_locked_ = previous_locked;
        master_seed_ = previous_master_seed;
        secureClearBytes(previous_master_seed);

        // Close the wallet DB (will be reopened by open())
        sqlite3_close(new_wallet_db);
        new_wallet_db = nullptr;

        // Register wallet in registry
        std::vector<uint8_t> empty_fingerprint; // Will be set later during HD wallet creation
        if (!registerWalletInRegistry(cleanName, walletPathStr, "mainnet", false, empty_fingerprint)) {
            throw std::runtime_error("Failed to register wallet in registry");
        }

        WLOG_INFO("[CREATE] ✅ Wallet created successfully: " + cleanName);

        // Automatically open the newly created wallet
        open(cleanName);

        if (!HaveMasterSeed()) {
            throw std::runtime_error("Invariant violation: new wallet opened without initialized HD master seed");
        }

        g_logger.info("Created wallet: " + cleanName);

    } catch (...) {
        // Cleanup on error
        if (new_wallet_db) {
            sqlite3_close(new_wallet_db);
        }
        // Remove wallet file if creation failed
        if (std::filesystem::exists(walletPath)) {
            std::filesystem::remove(walletPath);
        }
        throw;
    }
}

void WalletManager::open(const std::string& name) {
    WLOG_INFO("[OPEN] Opening wallet: " + name);

    // Check if wallet exists in registry
    if (!exists(name)) {
        throw std::runtime_error("Wallet not found in registry: " + name);
    }

    // Get wallet path from registry
    std::string walletPath = getWalletPathFromRegistry(name);
    if (walletPath.empty()) {
        throw std::runtime_error("Wallet path not found in registry: " + name);
    }

    WLOG_INFO("[OPEN] Wallet database path: " + walletPath);

    // On iOS the app container UUID changes on reinstall/rebuild. The old
    // container can remain readable briefly, so "stored path still exists" is
    // NOT evidence that it belongs to the current app container. Prefer the
    // canonical wallet file under the current dataDir_ whenever it exists and
    // repair the registry even if the stale absolute path also still exists.
    const std::filesystem::path expected =
        dataDir_ / "wallets" / ("wallet_" + name + ".db");
    if (std::filesystem::exists(expected) &&
        std::filesystem::path(walletPath).lexically_normal() != expected.lexically_normal()) {
        WLOG_WARN("[OPEN] Registry path belongs to an old data directory; repairing to: " +
                  expected.string());
        walletPath = expected.string();
        updateWalletPathInRegistry(name, walletPath);
    } else if (!std::filesystem::exists(walletPath)) {
        throw std::runtime_error("Wallet database file not found: " + walletPath);
    }

    // Close current wallet if open
    if (db_) {
        WLOG_INFO("[OPEN] Closing currently open wallet: " + current_);
        close();
    }

    // Open the wallet database file
    auto opened = open_sqlite(walletPath);
    if (opened.rc != SQLITE_OK) {
        throw std::runtime_error("Cannot open wallet database: " + opened.errmsg);
    }
    db_ = opened.db;

    WLOG_INFO("[OPEN] Wallet database opened successfully");

    // Log database state
    DbProbe::afterOpen(db_, walletPath);
    attachSqliteTrace(db_);

    // Initialize database (sets PRAGMAs, runs health checks), then reject
    // stale pre-v7 descriptor/path metadata before this wallet is considered loaded.
    try {
        initializeDatabase();
        assertNoRetiredLegacyCoinTypeInWalletDatabase(name);
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }

    // Set current wallet
    current_ = name;
    current_wallet_id_ = 1;  // Per-wallet DB always has id=1 in wallet_meta

    // Update last_opened in registry
    updateLastOpened(name);

    // Load blockchain height from database
    loadBlockchainHeight();

    // Check encryption status from encryption_metadata table
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT encrypted FROM encryption_metadata WHERE id = 1 LIMIT 1";

    wallet_encrypted_ = false;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            wallet_encrypted_ = (sqlite3_column_int(stmt, 0) == 1);
        }
        sqlite3_finalize(stmt);
    }

    wallet_locked_ = wallet_encrypted_;  // Start locked if encrypted

    g_logger.info("[WALLET OPEN DEBUG] wallet_encrypted_ = " +
                  std::string(wallet_encrypted_ ? "true" : "false") +
                  ", wallet_locked_ = " +
                  std::string(wallet_locked_ ? "true" : "false"));

    if (wallet_encrypted_) {
        g_logger.info("Opened encrypted wallet (locked): " + name);
        g_logger.info("[WALLET OPEN DEBUG] Wallet is ENCRYPTED and LOCKED");
    } else {
        g_logger.info("Opened unencrypted wallet: " + name);
        g_logger.info("[WALLET OPEN DEBUG] Wallet is UNENCRYPTED, attempting to load master seed...");

        // For unencrypted wallets, load the master seed immediately
        auto seed_opt = loadMasterSeed("");  // Empty passphrase for unencrypted
        if (seed_opt) {
            master_seed_ = seed_opt.value();
            g_logger.info("[WALLET OPEN DEBUG] SUCCESS: Loaded master seed (" +
                          std::to_string(master_seed_.size()) + " bytes)");
            WLOG_INFO("✅ Auto-loaded HD master seed for unencrypted wallet");
        } else {
            g_logger.error("[WALLET OPEN DEBUG] FAILED: loadMasterSeed(\"\") returned nullopt");
            WLOG_WARN("No HD master seed found (wallet may not be HD wallet)");
        }
    }

    // Primary addresses are computed lazily on first getPrimaryAddress() call
    // (via wallet.getinfo). Not done here to avoid blocking wallet.create flow.

    WLOG_INFO("[OPEN] ✅ Wallet opened successfully: " + name);

    // Initialize Lightning for this wallet - DISABLED: Lightning is standalone
    // if (lightning_service_) {
    //     if (!lightning_service_->InitForWallet(this)) {
    //         g_logger.warning("⚡ Failed to initialize Lightning for wallet: " + name + " (non-fatal)");
    //     }
    // }
}

void WalletManager::rename(const std::string& oldName, const std::string& newName) {
    const std::string cleanNewName = sanitize(newName);
    if (cleanNewName.empty()) {
        throw std::invalid_argument("Invalid new wallet name");
    }
    
    if (!exists(oldName)) {
        throw std::runtime_error("Wallet not found: " + oldName);
    }
    
    if (exists(cleanNewName)) {
        throw std::runtime_error("Wallet already exists: " + cleanNewName);
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "UPDATE wallets SET name = ? WHERE name = ?";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare wallet rename: " + std::string(sqlite3_errmsg(db_)));
    }
    
    sqlite3_bind_text(stmt, 1, cleanNewName.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, oldName.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to rename wallet: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Update current wallet name if it was the renamed one
    if (current_ == oldName) {
        current_ = cleanNewName;
    }
    
    g_logger.info("Renamed wallet: " + oldName + " -> " + cleanNewName);
}

void WalletManager::remove(const std::string& name) {
    if (!exists(name)) {
        throw std::runtime_error("Wallet not found: " + name);
    }

    if (current_ == name) {
        throw std::runtime_error("Cannot delete currently active wallet");
    }

    if (!registry_db_) {
        throw std::runtime_error("Wallet registry not available");
    }

    // Resolve wallet DB file from registry.
    std::string wallet_path = getWalletPathFromRegistry(name);
    if (wallet_path.empty()) {
        throw std::runtime_error("Wallet path not found in registry: " + name);
    }

    // Safety check: don't delete wallets with unspent UTXOs.
    int utxo_count = 0;
    sqlite3* wallet_db = nullptr;
    if (sqlite3_open(wallet_path.c_str(), &wallet_db) == SQLITE_OK) {
        sqlite3_stmt* utxo_stmt = nullptr;
        const char* utxo_sql = "SELECT COUNT(*) FROM utxos WHERE is_spent = 0";
        if (sqlite3_prepare_v2(wallet_db, utxo_sql, -1, &utxo_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(utxo_stmt) == SQLITE_ROW) {
                utxo_count = sqlite3_column_int(utxo_stmt, 0);
            }
            sqlite3_finalize(utxo_stmt);
        }
        sqlite3_close(wallet_db);
    }

    if (utxo_count > 0) {
        throw std::runtime_error("Cannot delete wallet with unspent UTXOs (" + std::to_string(utxo_count) + ")");
    }

    // Delete wallet record from registry.
    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    const char* delete_sql = "DELETE FROM wallets WHERE name = ?";
    rc = sqlite3_prepare_v2(registry_db_, delete_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare wallet deletion: " + std::string(sqlite3_errmsg(registry_db_)));
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to delete wallet from registry: " + std::string(sqlite3_errmsg(registry_db_)));
    }

    // Delete wallet files from disk.
    std::error_code ec;
    std::filesystem::remove(wallet_path, ec);
    std::filesystem::remove(wallet_path + "-wal", ec);
    std::filesystem::remove(wallet_path + "-shm", ec);
    if (ec) {
        WLOG_WARN("Deleted wallet from registry but failed to remove some files: " + wallet_path + " (" + ec.message() + ")");
    }

    g_logger.info("Deleted wallet: " + name);
}

// Validate Dinero bech32 address format
bool WalletManager::isValidDineroBech32(const std::string& addr) const {
    // Accept mainnet (din1), testnet (tdin1), and regtest (rdin1) addresses
    bool validPrefix = (addr.rfind("din1", 0) == 0) ||
                      (addr.rfind("tdin1", 0) == 0) ||
                      (addr.rfind("rdin1", 0) == 0);
    return validPrefix && addr.size() >= 20 && addr.size() <= 90;
}

void WalletManager::setAddressLabel(const std::string& addr, const std::string& label, bool is_system) {
    if (current_wallet_id_ == -1) {
        throw std::runtime_error("No wallet is currently open");
    }

    // Validate address format
    if (!isValidDineroBech32(addr)) {
        throw std::runtime_error("Invalid address format (must be din1..., tdin1..., or rdin1...)");
    }

    sqlite3_stmt* stmt;

    // First try to update existing HD address
    // Only update if: setting user label OR address has no label yet OR it's a system label being overwritten by system
    const char* update_hd_sql = is_system
        ? "UPDATE addresses SET label = ? WHERE address = ? AND wallet_id = ? AND (label IS NULL OR label = '' OR is_system_label = 1)"
        : "UPDATE addresses SET label = ?, is_system_label = 0 WHERE address = ? AND wallet_id = ?";
    int rc = sqlite3_prepare_v2(db_, update_hd_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare HD label update: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, label.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, addr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, current_wallet_id_);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to set HD address label: " + std::string(sqlite3_errmsg(db_)));
    }

    // If system label was set successfully, also set the is_system_label flag
    if (is_system && sqlite3_changes(db_) > 0) {
        const char* set_system_sql = "UPDATE addresses SET is_system_label = 1 WHERE address = ? AND wallet_id = ?";
        rc = sqlite3_prepare_v2(db_, set_system_sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, addr.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, current_wallet_id_);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // If no HD address was updated, upsert into address book
    if (sqlite3_changes(db_) == 0) {
        const char* upsert_sql = R"(
            INSERT INTO wallet_addresses(wallet_id, address, label)
            VALUES(?, ?, ?)
            ON CONFLICT(wallet_id, address) DO UPDATE SET label=excluded.label
        )";

        rc = sqlite3_prepare_v2(db_, upsert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare address book upsert: " + std::string(sqlite3_errmsg(db_)));
        }

        sqlite3_bind_int(stmt, 1, current_wallet_id_);
        sqlite3_bind_text(stmt, 2, addr.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, label.c_str(), -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            throw std::runtime_error("Failed to upsert address book entry: " + std::string(sqlite3_errmsg(db_)));
        }
    }
}

void WalletManager::addHDAddress(const std::string& addr, int account, int change, int index, const std::string& label) {
    if (!db_) {
        throw std::runtime_error("No wallet database is currently open");
    }

    // Validate address format
    if (!isValidDineroBech32(addr)) {
        throw std::runtime_error("Invalid address format (must be din1..., tdin1..., or rdin1...)");
    }

    sqlite3_stmt* stmt;
    const bool addresses_has_wallet_id = columnExists(db_, "addresses", "wallet_id");
    const char* sql = addresses_has_wallet_id
        ? R"(
            INSERT OR REPLACE INTO addresses(wallet_id, account, change, idx, address, label, type)
            VALUES(?, ?, ?, ?, ?, ?, 'p2wpkh')
        )"
        : R"(
            INSERT OR REPLACE INTO addresses(account, change, idx, address, label, type)
            VALUES(?, ?, ?, ?, ?, 'p2wpkh')
        )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare HD address insert: " + std::string(sqlite3_errmsg(db_)));
    }

    int bind_index = 1;
    if (addresses_has_wallet_id) {
        sqlite3_bind_int(stmt, bind_index++, 1);
    }
    sqlite3_bind_int(stmt, bind_index++, account);
    sqlite3_bind_int(stmt, bind_index++, change);
    sqlite3_bind_int(stmt, bind_index++, index);
    sqlite3_bind_text(stmt, bind_index++, addr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, bind_index++, label.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert HD address: " + std::string(sqlite3_errmsg(db_)));
    }
}

int WalletManager::getNextAddressIndex(int account, int change) const {
    if (current_wallet_id_ == -1) {
        throw std::runtime_error("No wallet is currently open");
    }

    sqlite3_stmt* stmt;
    // Per-wallet database: addresses table has no wallet_id column
    const char* sql = "SELECT COALESCE(MAX(idx), -1) + 1 FROM addresses WHERE account = ? AND change = ?";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare next index query: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_int(stmt, 1, account);
    sqlite3_bind_int(stmt, 2, change);
    
    int next_index = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        next_index = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to get next address index: " + std::string(sqlite3_errmsg(db_)));
    }
    
    return next_index;
}

bool WalletManager::isAddressMine(const std::string& addr) const {
    if (current_wallet_id_ == -1) {
        WLOG_WARN("❌ isAddressMine: current_wallet_id_ is -1 (not set)");
        return false;
    }

    if (!db_) {
        WLOG_WARN("❌ isAddressMine: db_ is null");
        return false;
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT 1 FROM addresses WHERE wallet_id = ? AND address = ? LIMIT 1";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        WLOG_WARN("❌ isAddressMine: SQL prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    sqlite3_bind_int(stmt, 1, current_wallet_id_);
    sqlite3_bind_text(stmt, 2, addr.c_str(), -1, SQLITE_STATIC);

    WLOG_DEBUG("🔍 isAddressMine: Checking wallet_id=" + std::to_string(current_wallet_id_) + " for address=" + addr);

    bool is_mine = false;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        is_mine = true;
        WLOG_DEBUG("✅ isAddressMine: Address found in wallet");
    } else if (rc != SQLITE_DONE) {
        WLOG_WARN("❌ isAddressMine: Query failed: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_finalize(stmt);

    if (is_mine) {
        return true;
    }

    const std::string p2mr_store_path = GetV7P2MRStorePath();
    if (!p2mr_store_path.empty()) {
        wallet::V7P2MRStore p2mr_store;
        if (p2mr_store.Open(p2mr_store_path) == wallet::V7P2MRStore::OpenResult::Ok &&
            p2mr_store.GetByAddress(current_wallet_id_, addr).has_value()) {
            WLOG_DEBUG("✅ isAddressMine: Address found in v7 P2MR store");
            return true;
        }
    }

    WLOG_DEBUG("❌ isAddressMine: Address NOT found in legacy or v7 stores");
    return false;
}

bool WalletManager::isScriptMine(const std::string& script_pubkey) const {
    if (current_wallet_id_ == -1 || !db_) {
        return false;
    }

    // 1) Primary ownership source: addresses table (scriptPubKey text).
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT 1 FROM addresses WHERE script_pubkey = ? LIMIT 1";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, script_pubkey.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_ROW) {
            return true;
        }
    }

    // 2) Fallback ownership source: watch_scripts table (scriptPubKey BLOB).
    // This is critical for iOS NodeCore watch registration, which can track scripts
    // that are not yet present in the addresses table.
    if (script_pubkey.empty() || (script_pubkey.size() % 2) != 0) {
        return false;
    }

    std::vector<uint8_t> script_bytes;
    script_bytes.reserve(script_pubkey.size() / 2);
    for (size_t i = 0; i < script_pubkey.size(); i += 2) {
        unsigned int byte = 0;
        if (std::sscanf(script_pubkey.c_str() + i, "%2x", &byte) != 1) {
            return false;
        }
        script_bytes.push_back(static_cast<uint8_t>(byte));
    }

    sqlite3_stmt* watch_stmt = nullptr;
    const char* watch_sql = "SELECT 1 FROM watch_scripts WHERE script_pubkey = ? LIMIT 1";
    int watch_rc = sqlite3_prepare_v2(db_, watch_sql, -1, &watch_stmt, nullptr);
    if (watch_rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_blob(watch_stmt, 1, script_bytes.data(), static_cast<int>(script_bytes.size()), SQLITE_STATIC);
    watch_rc = sqlite3_step(watch_stmt);
    sqlite3_finalize(watch_stmt);

    return watch_rc == SQLITE_ROW;
}

std::optional<std::string> WalletManager::getAddressLabel(const std::string& addr) const {
    if (current_wallet_id_ == -1) {
        return std::nullopt;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT label FROM addresses WHERE address = ? AND wallet_id = ?";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::nullopt;
    }
    
    sqlite3_bind_text(stmt, 1, addr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, current_wallet_id_);
    
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (label) {
            result = std::string(label);
        }
    }
    
    sqlite3_finalize(stmt);
    return result;
}

bool WalletManager::isSystemLabel(const std::string& addr) const {
    if (current_wallet_id_ == -1) {
        return false;
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT is_system_label FROM addresses WHERE address = ? AND wallet_id = ?";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, addr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, current_wallet_id_);

    bool is_system = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        is_system = sqlite3_column_int(stmt, 0) == 1;
    }

    sqlite3_finalize(stmt);
    return is_system;
}

std::vector<AddressRow> WalletManager::listAddresses(bool includeLabels) const {
    std::vector<AddressRow> addresses;
    
    if (current_wallet_id_ == -1) {
        return addresses;
    }
    
    sqlite3_stmt* stmt;
    
    // Per-wallet database: addresses table has no wallet_id column
    // Simplified query - just get addresses from addresses table
    const char* sql = R"(
        SELECT address,
               COALESCE(label,'') AS label,
               account, change, idx, 0 AS external,
               COALESCE(type,'p2wpkh') AS type,
               COALESCE(script_pubkey,'') AS script_pubkey
          FROM addresses
         ORDER BY change, account, idx, address
    )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return addresses;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        // SEATBELT: Validate address before use
        const char* addr_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!addr_cstr || strlen(addr_cstr) == 0) {
            logCorruptRow("addresses", "address", "NULL or empty address");
            continue;  // Skip this row, continue with others
        }

        AddressRow row;
        row.address = addr_cstr;

        if (includeLabels) {
            const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (label) {
                row.label = std::string(label);
            }
        }

        row.account = sqlite3_column_int(stmt, 2);
        row.change = sqlite3_column_int(stmt, 3);
        row.index = sqlite3_column_int(stmt, 4);
        row.external = sqlite3_column_int(stmt, 5) != 0;

        // Get address type (p2wpkh or p2tr)
        const char* type_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        if (type_str) {
            row.type = std::string(type_str);
        }

        // Get scriptPubKey (Bitcoin-grade: use scriptPubKey for ownership, not address strings)
        const char* script_pubkey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (script_pubkey) {
            row.script_pubkey = std::string(script_pubkey);
        }

        addresses.push_back(std::move(row));
    }
    
    sqlite3_finalize(stmt);
    return addresses;
}

void WalletManager::removeAddress(const std::string& addr) {
    if (current_wallet_id_ == -1) {
        throw std::runtime_error("No wallet is currently open");
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "DELETE FROM wallet_addresses WHERE wallet_id = ? AND address = ?";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare address removal: " + std::string(sqlite3_errmsg(db_)));
    }
    
    sqlite3_bind_int(stmt, 1, current_wallet_id_);
    sqlite3_bind_text(stmt, 2, addr.c_str(), -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to remove address: " + std::string(sqlite3_errmsg(db_)));
    }
    
    // Note: We only remove from wallet_addresses (address book), not from addresses (HD addresses)
    // HD addresses should not be removable as they are part of the wallet's derivation
}

void WalletManager::close() {
    if (utxo_index_) {
        utxo_index_->ClearRegisteredAddresses();
    }

    // Scrub sensitive material whenever a wallet is closed/destroyed.
    clearPrivateKeyCache();
    secureClearBytes(master_seed_);
    secureClearString(encryption_key_);
    primary_address_.clear();
    wallet_locked_ = true;
    unlock_timeout_ = 0;
    unlock_time_ = 0;

    // Stop Lightning for this wallet before closing database - DISABLED: Lightning is standalone
    // if (lightning_service_) {
    //     lightning_service_->StopForWallet();
    // }

    // HDWallet is injected by WalletService and remains owned there.
    hd_wallet_ = nullptr;

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    current_.clear();
    current_wallet_id_ = -1;
}

void WalletManager::closeRegistry() {
    if (registry_db_) {
        sqlite3_close(registry_db_);
        registry_db_ = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════════
// Wallet Registry Helper Methods
// ═══════════════════════════════════════════════════════════════

bool WalletManager::registerWalletInRegistry(
    const std::string& name,
    const std::string& path,
    const std::string& network,
    bool encrypted,
    const std::vector<uint8_t>& fingerprint
) {
    if (!registry_db_) {
        WLOG_ERR("Registry database not open");
        return false;
    }

    try {
        sqlite3_stmt* stmt = nullptr;
        // Metadata refreshes must not use INSERT OR REPLACE. SQLite implements
        // REPLACE as delete-then-insert, which changes the registry row id and
        // clears last_opened. That can make a different wallet active after a
        // clean restart even though this wallet was the last one opened.
        const char* sql = R"(
            INSERT INTO wallets (name, path, network, encrypted, fingerprint, created_at)
            VALUES (?, ?, ?, ?, ?, strftime('%s','now'))
            ON CONFLICT(name) DO UPDATE SET
                path = excluded.path,
                network = excluded.network,
                encrypted = excluded.encrypted,
                fingerprint = excluded.fingerprint
        )";

        if (sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            WLOG_ERR("Failed to prepare registry insert: " + std::string(sqlite3_errmsg(registry_db_)));
            return false;
        }

        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, network.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, encrypted ? 1 : 0);

        if (!fingerprint.empty()) {
            sqlite3_bind_blob(stmt, 5, fingerprint.data(), fingerprint.size(), SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 5);
        }

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            WLOG_ERR("Failed to register wallet in registry: " + std::string(sqlite3_errmsg(registry_db_)));
            return false;
        }

        WLOG_INFO("✅ Registered wallet in registry: " + name);
        return true;

    } catch (const std::exception& e) {
        WLOG_ERR("Exception while registering wallet: " + std::string(e.what()));
        return false;
    }
}

std::string WalletManager::getWalletPathFromRegistry(const std::string& name) const {
    if (!registry_db_) {
        WLOG_ERR("Registry database not open");
        return "";
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT path FROM wallets WHERE name = ? LIMIT 1";

    if (sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        WLOG_ERR("Failed to prepare registry query");
        return "";
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);

    std::string path;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* path_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        // SEATBELT: Log and return empty if path is corrupt
        if (!path_cstr || strlen(path_cstr) == 0) {
            logCorruptRow("wallets", "path", ("NULL or empty path for wallet: " + name).c_str());
            // Return empty - caller will handle as "wallet not found"
        } else {
            path = path_cstr;
        }
    }

    sqlite3_finalize(stmt);
    return path;
}

void WalletManager::updateWalletPathInRegistry(const std::string& name, const std::string& newPath) {
    if (!registry_db_) return;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE wallets SET path = ? WHERE name = ?";
    if (sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, newPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void WalletManager::updateLastOpened(const std::string& name) {
    if (!registry_db_) {
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE wallets SET last_opened = strftime('%s','now') WHERE name = ?";

    if (sqlite3_prepare_v2(registry_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string WalletManager::sanitize(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') {
            s += c;
        }
    }
    
    // Trim spaces
    auto l = s.find_first_not_of(' ');
    auto r = s.find_last_not_of(' ');
    if (l == std::string::npos) return {};
    s = s.substr(l, r - l + 1);
    
    // Collapse runs of spaces
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (c == ' ') {
            if (!space) {
                out.push_back(' ');
                space = true;
            }
        } else {
            out.push_back(c);
            space = false;
        }
    }
    
    // Limit length and avoid reserved names
    if (out.length() > 64) {
        out = out.substr(0, 64);
    }
    
    // Avoid Windows reserved names
    static const std::vector<std::string> reserved = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    
    std::string upper = out;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    
    for (const auto& res : reserved) {
        if (upper == res) {
            out += "_";
            break;
        }
    }
    
    return out;
}

void WalletManager::unload() {
    if (!hasActiveWallet()) {
        return;
    }
    WLOG_INFO("[UNLOAD] Unloading wallet: " + current_);
    close();
}

int WalletManager::getWalletId(const std::string& name) const {
    sqlite3_stmt* stmt;
    const char* sql = "SELECT id FROM wallets WHERE name = ?";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
    
    int wallet_id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        wallet_id = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return wallet_id;
}

void WalletManager::setCurrentWallet(const std::string& name, int wallet_id) {
    current_ = name;
    current_wallet_id_ = wallet_id;
}

// Static SQLite helper methods
void WalletManager::exec(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string error = err_msg ? err_msg : "Unknown SQLite error";
        sqlite3_free(err_msg);
        throw std::runtime_error("SQLite error: " + error);
    }
}

int WalletManager::getUserVersion(sqlite3* db) {
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;
    
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return version;
}

void WalletManager::setUserVersion(sqlite3* db, int version) {
    std::string sql = "PRAGMA user_version = " + std::to_string(version);
    exec(db, sql.c_str());
}

bool WalletManager::tableExists(sqlite3* db, const char* name) {
    sqlite3_stmt* stmt;
    const char* sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    
    sqlite3_finalize(stmt);
    return exists;
}

bool WalletManager::columnExists(sqlite3* db, const char* table, const char* col) {
    std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    
    bool exists = false;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string expected_col = toLower(col ? col : "");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* column_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (column_name && toLower(column_name) == expected_col) {
            exists = true;
            break;
        }
    }
    
    sqlite3_finalize(stmt);
    return exists;
}

void WalletManager::assertNoRetiredLegacyCoinTypeInWalletDatabase(const std::string& wallet_name) const {
    if (!db_) {
        return;
    }

    struct ScanTarget {
        const char* table;
        const char* column;
    };

    const ScanTarget targets[] = {
        {"imported_descriptors", "descriptor"},
        {"imported_descriptors", "derivation_path_prefix"},
        {"address_derivation_paths", "derivation_path"},
        {"taproot_key_mapping", "derivation_path"},
        {"wallet_keys", "derivation_path"},
        {"wallet_addresses", "derivation_path"},
    };

    for (const auto& target : targets) {
        if (!tableExists(db_, target.table) || !columnExists(db_, target.table, target.column)) {
            continue;
        }

        const std::string sql =
            std::string("SELECT ") + target.column + " FROM " + target.table +
            " WHERE " + target.column + " LIKE '%" +
            std::to_string(dinero::wallet::RETIRED_LEGACY_COIN_TYPE) + "%' LIMIT 100";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to scan wallet for retired coin type: " +
                                     std::string(sqlite3_errmsg(db_)));
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const std::string value = value_cstr ? value_cstr : "";
            if (dinero::wallet::TextContainsRetiredLegacyCoinTypePathComponent(value)) {
                sqlite3_finalize(stmt);
                throw std::runtime_error(
                    "Refusing to load wallet '" + wallet_name + "': " +
                    dinero::wallet::RetiredLegacyCoinTypeError(
                        std::string("retired path found in ") +
                        target.table + "." + target.column) +
                    ". "
                    "restore/rederive with coin_type 1448."
                );
            }
        }

        sqlite3_finalize(stmt);
    }
}

sqlite3* WalletManager::getCurrentDatabase() const {
    return db_;
}

void WalletManager::setSetting(const std::string& key, const std::string& value, const std::string& wallet, const std::string& network) {
    if (!db_) throw std::runtime_error("Database not initialized");

    // NOTE: wallet and network parameters are ignored - per-wallet database provides implicit context
    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT OR REPLACE INTO settings (key, value, updated_at)
        VALUES (?, ?, strftime('%s','now'))
    )";

    if (!SqlLog::prepare(&stmt, db_, sql, "settings-upsert")) {
        throw std::runtime_error("Failed to prepare settings upsert");
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_STATIC);

    if (!SqlLog::exec(stmt, "settings-upsert")) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to set setting: " + key);
    }

    sqlite3_finalize(stmt);
}

std::string WalletManager::getSetting(const std::string& key, const std::string& wallet, const std::string& network) const {
    if (!db_) return "";
    
    sqlite3_stmt* stmt;
    // Per-wallet database: settings table only has key, value columns (no wallet, network)
    const char* sql = "SELECT value FROM settings WHERE key = ? LIMIT 1";

    if (!SqlLog::prepare(&stmt, db_, sql, "settings-get")) {
        return "";
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (value) result = value;
    }
    
    sqlite3_finalize(stmt);
    return result;
}

bool WalletManager::hasSetting(const std::string& key, const std::string& wallet, const std::string& network) const {
    return !getSetting(key, wallet, network).empty();
}

void WalletManager::setMiningAddress(const std::string& address, const std::string& wallet, const std::string& network) {
    // Validate address ownership if wallet is specified
    if (!wallet.empty() && !isAddressMine(address)) {
        throw std::runtime_error("Address not owned by wallet: " + wallet);
    }
    
    // Set mining address (wallet context is implicit in per-wallet database)
    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT OR REPLACE INTO settings (key, value, updated_at)
        VALUES ('mining_address', ?, strftime('%s','now'))
    )";

    if (!SqlLog::prepare(&stmt, db_, sql, "mining-address-set")) {
        throw std::runtime_error("Failed to prepare mining address setting");
    }

    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);

    if (!SqlLog::exec(stmt, "mining-address-set")) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Failed to set mining address");
    }

    sqlite3_finalize(stmt);
}

std::string WalletManager::getMiningAddress(const std::string& wallet, const std::string& network) const {
    return getSetting("mining_address", wallet, network);
}

// Wallet encryption/decryption methods
void WalletManager::encryptWallet(const std::string& passphrase) {
    if (wallet_encrypted_) {
        throw std::runtime_error("Wallet is already encrypted");
    }

    if (passphrase.empty()) {
        throw std::runtime_error("Passphrase cannot be empty");
    }

    // Generate salt and derive encryption key
    unsigned char salt[32];
    if (!CF_GenerateRandomBytes(salt, sizeof(salt))) {
        throw std::runtime_error("Failed to generate random salt");
    }

    // Keep binary salt for key derivation
    std::string saltStr(reinterpret_cast<char*>(salt), sizeof(salt));
    encryption_key_ = deriveKey(passphrase, saltStr);

    // Create a verification hash to validate password on unlock
    // Hash the derived key to create a verification value
    uint8_t verification_hash[32];
    ::sha256(
        reinterpret_cast<const uint8_t*>(encryption_key_.data()),
        encryption_key_.size(),
        verification_hash
    );

    // Convert binary data to hex strings for storage (prevents null-byte truncation)
    std::vector<unsigned char> saltVec(salt, salt + sizeof(salt));
    std::vector<unsigned char> verifyVec(verification_hash, verification_hash + sizeof(verification_hash));
    std::string saltHex = util::hex(saltVec);
    std::string verifyHex = util::hex(verifyVec);

    // Store encryption metadata (hex-encoded to prevent null-byte corruption)
    setSetting("wallet_encrypted", "1");
    setSetting("wallet_salt", saltHex);
    setSetting("wallet_verify_hash", verifyHex);

    wallet_encrypted_ = true;
    wallet_locked_ = false;  // Keep unlocked during initial encryption to generate/re-encrypt HD seed

    // ═══════════════════════════════════════════════════════════════
    // Phase 5: Generate and store HD wallet master seed
    // ═══════════════════════════════════════════════════════════════

    // Per-wallet DB stores a single seed row at id=1.
    sqlite3_stmt* stmt = nullptr;
    const char* check_sql = "SELECT COUNT(*) FROM hd_seeds WHERE id = 1";
    int seed_exists = 0;

    if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            seed_exists = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    std::vector<uint8_t> seed_to_store;

    if (!master_seed_.empty()) {
        seed_to_store = master_seed_;
        WLOG_INFO("Using in-memory HD seed for wallet encryption");
    } else if (seed_exists > 0) {
        auto existing_seed = loadMasterSeed("");
        if (existing_seed.has_value()) {
            seed_to_store = std::move(existing_seed.value());
            WLOG_INFO("Loaded existing HD seed for wallet encryption");
        } else {
            WLOG_WARN("HD seed exists but could not be decrypted with empty passphrase");
        }
    }

    if (seed_to_store.empty()) {
        throw std::runtime_error(
            "Cannot encrypt wallet: no HD seed found. "
            "Create or restore a wallet first before encrypting.");
    }

    // Re-encrypt the current seed with the user passphrase.
    // Do not reset address/UTXO tables during wallet encryption.
    if (!storeMasterSeed(seed_to_store, passphrase, kResetAddressStateDuringEncryption)) {
        throw std::runtime_error("Failed to store encrypted HD master seed");
    }

    // Keep the seed in memory until we lock at the end of this method.
    master_seed_ = seed_to_store;
    WLOG_INFO("✅ HD wallet master seed encrypted with wallet passphrase");

    // ═══════════════════════════════════════════════════════════════
    // CRITICAL FIX: Update encryption_metadata table
    // ═══════════════════════════════════════════════════════════════
    // This table is read by open() method to determine wallet lock state.
    // Must be updated even if HD seed already existed (and storeMasterSeed wasn't called).

    const char* meta_sql = R"(
        INSERT OR REPLACE INTO encryption_metadata (
            id, encrypted, kdf, kdf_iterations, cipher, salt, created_at, updated_at
        )
        VALUES (1, 1, 'pbkdf2-hmac-sha512', 600000, 'AES-256-GCM', NULL,
                strftime('%s','now'), strftime('%s','now'))
    )";

    sqlite3_stmt* meta_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, meta_sql, -1, &meta_stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare encryption_metadata update");
    }

    int rc = sqlite3_step(meta_stmt);
    sqlite3_finalize(meta_stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to update encryption_metadata table");
    }

    // Lock wallet after encryption (Phase E.1.2 Security Policy)
    // Newly encrypted wallets should be locked by default
    lockWallet();

    WLOG_INFO("Wallet encrypted and locked successfully");
}

void WalletManager::decryptWallet(const std::string& passphrase) {
    if (!wallet_encrypted_) {
        throw std::runtime_error("Wallet is not encrypted");
    }
    
    // Verify passphrase by attempting to derive key
    std::string salt = getSetting("wallet_salt");
    std::string derivedKey = deriveKey(passphrase, salt);
    
    if (derivedKey != encryption_key_) {
        throw std::runtime_error("Invalid passphrase");
    }
    
    // Remove encryption metadata
    setSetting("wallet_encrypted", "");
    setSetting("wallet_salt", "");
    
    wallet_encrypted_ = false;
    wallet_locked_ = false;
    secureClearString(encryption_key_);
    
    WLOG_INFO("Wallet decrypted successfully");
}

void WalletManager::changePassphrase(const std::string& oldPassphrase, const std::string& newPassphrase) {
    if (!wallet_encrypted_) {
        throw std::runtime_error("Wallet is not encrypted");
    }

    if (newPassphrase.empty()) {
        throw std::runtime_error("New passphrase cannot be empty");
    }

    // Get hex-encoded salt and decode to binary
    std::string saltHex = getSetting("wallet_salt");
    std::vector<unsigned char> saltBytes;
    if (!util::unhex(saltHex, saltBytes) || saltBytes.size() != 32) {
        throw std::runtime_error("Invalid wallet salt (corrupted or missing)");
    }
    std::string salt(reinterpret_cast<char*>(saltBytes.data()), saltBytes.size());

    // Derive key from old passphrase
    std::string oldKey = deriveKey(oldPassphrase, salt);

    // Validate old passphrase against stored verification hash
    std::string storedVerifyHex = getSetting("wallet_verify_hash");
    if (!storedVerifyHex.empty()) {
        // Decode stored verification hash from hex
        std::vector<unsigned char> storedVerifyBytes;
        if (!util::unhex(storedVerifyHex, storedVerifyBytes) || storedVerifyBytes.size() != 32) {
            throw std::runtime_error("Invalid wallet verification hash (corrupted)");
        }

        // Compute hash of derived key
        uint8_t computed_hash[32];
        ::sha256(
            reinterpret_cast<const uint8_t*>(oldKey.data()),
            oldKey.size(),
            computed_hash
        );

        // Compare hashes
        if (std::memcmp(computed_hash, storedVerifyBytes.data(), 32) != 0) {
            throw std::runtime_error("Invalid old passphrase");
        }
    }

    // Generate new salt and key
    unsigned char newSalt[32];
    if (!CF_GenerateRandomBytes(newSalt, sizeof(newSalt))) {
        throw std::runtime_error("Failed to generate random salt");
    }

    // Keep binary salt for key derivation
    std::string newSaltStr(reinterpret_cast<char*>(newSalt), sizeof(newSalt));
    encryption_key_ = deriveKey(newPassphrase, newSaltStr);

    // Create new verification hash
    uint8_t new_verification_hash[32];
    ::sha256(
        reinterpret_cast<const uint8_t*>(encryption_key_.data()),
        encryption_key_.size(),
        new_verification_hash
    );

    // Convert new binary data to hex for storage
    std::vector<unsigned char> newSaltVec(newSalt, newSalt + sizeof(newSalt));
    std::vector<unsigned char> newVerifyVec(new_verification_hash, new_verification_hash + sizeof(new_verification_hash));
    std::string newSaltHex = util::hex(newSaltVec);
    std::string newVerifyHex = util::hex(newVerifyVec);

    // Update stored salt and verification hash (hex-encoded)
    setSetting("wallet_salt", newSaltHex);
    setSetting("wallet_verify_hash", newVerifyHex);

    WLOG_INFO("Wallet passphrase changed successfully");
}

std::string WalletManager::getPrimaryAddress() {
    if (primary_address_.empty()) {
        try { derivePrimaryAddresses(); }
        catch (...) {}
    }
    return primary_address_;
}

void WalletManager::derivePrimaryAddresses() {
    // Compute primary transparent address (BIP86, account 0, index 0)
    if (primary_address_.empty()) {
        try {
            // Try: index 0 address from addresses table
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT address FROM addresses WHERE account_index = 0 AND address_index = 0 AND change = 0 LIMIT 1";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (val) primary_address_ = val;
                }
                sqlite3_finalize(stmt);
            }

            // Fallback: any din1p address from the addresses table
            if (primary_address_.empty()) {
                stmt = nullptr;
                const char* sql2 = "SELECT address FROM addresses WHERE address LIKE 'din1p%' ORDER BY rowid LIMIT 1";
                if (sqlite3_prepare_v2(db_, sql2, -1, &stmt, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                        if (val) primary_address_ = val;
                    }
                    sqlite3_finalize(stmt);
                }
            }

            // Last resort: derive from seed
            if (primary_address_.empty() && HaveMasterSeed()) {
                primary_address_ = getNewAddress("", "taproot");
                WLOG_INFO("[PRIMARY] Derived primary transparent address: " + primary_address_);
            }
        } catch (const std::exception& e) {
            WLOG_WARN("[PRIMARY] Could not derive transparent address: " + std::string(e.what()));
        }
    }

}

void WalletManager::lockWallet() {
    if (!wallet_encrypted_) {
        throw std::runtime_error("Wallet is not encrypted");
    }

    wallet_locked_ = true;
    secureClearString(encryption_key_);
    clearPrivateKeyCache();
    secureClearBytes(master_seed_);
    // V7 PQ master key — scrub alongside v5 secrets.
    // See docs/consensus/V7_WALLET_SCHEMA.md §5b.
    OPENSSL_cleanse(pq_master_key_.data(), pq_master_key_.size());
    pq_master_key_loaded_ = false;
    unlock_timeout_ = 0;
    unlock_time_ = 0;

    WLOG_INFO("Wallet locked");
}

void WalletManager::unlockWallet(const std::string& passphrase, int timeoutSeconds) {
    if (!wallet_encrypted_) {
        throw std::runtime_error("Wallet is not encrypted");
    }

    // Get hex-encoded salt and decode to binary
    std::string saltHex = getSetting("wallet_salt");
    std::vector<unsigned char> saltBytes;
    if (!util::unhex(saltHex, saltBytes) || saltBytes.size() != 32) {
        throw std::runtime_error("Invalid wallet salt (corrupted or missing)");
    }
    std::string salt(reinterpret_cast<char*>(saltBytes.data()), saltBytes.size());

    // Derive key from passphrase using decoded binary salt
    std::string derivedKey = deriveKey(passphrase, salt);

    // Validate passphrase by checking against stored verification hash
    std::string storedVerifyHex = getSetting("wallet_verify_hash");
    bool needs_kdf_migration = false;

    if (!storedVerifyHex.empty()) {
        // Decode stored verification hash from hex
        std::vector<unsigned char> storedVerifyBytes;
        if (!util::unhex(storedVerifyHex, storedVerifyBytes) || storedVerifyBytes.size() != 32) {
            throw std::runtime_error("Invalid wallet verification hash (corrupted)");
        }

        // Compute hash of derived key
        uint8_t computed_hash[32];
        ::sha256(
            reinterpret_cast<const uint8_t*>(derivedKey.data()),
            derivedKey.size(),
            computed_hash
        );

        // Compare hashes
        if (std::memcmp(computed_hash, storedVerifyBytes.data(), 32) != 0) {
            // PBKDF2 key didn't match — try legacy HMAC-SHA512 (pre-v0.4.0 wallets)
            std::string legacyKey = deriveKeyLegacy(passphrase, salt);
            uint8_t legacy_hash[32];
            ::sha256(
                reinterpret_cast<const uint8_t*>(legacyKey.data()),
                legacyKey.size(),
                legacy_hash
            );

            if (std::memcmp(legacy_hash, storedVerifyBytes.data(), 32) != 0) {
                throw std::runtime_error("Invalid passphrase");
            }

            // Legacy key matched — use it and flag for migration
            WLOG_INFO("Legacy HMAC-SHA512 wallet detected, will migrate to PBKDF2");
            derivedKey = legacyKey;
            needs_kdf_migration = true;
            OPENSSL_cleanse(legacy_hash, sizeof(legacy_hash));
        }
    }

    // Passphrase is valid, unlock the wallet
    encryption_key_ = derivedKey;
    wallet_locked_ = false;

    // Legacy HMAC wallet: keep using legacy key (wallet data was encrypted with it)
    // Migration happens naturally when user changes their password via changePassphrase()
    if (needs_kdf_migration) {
        WLOG_WARN("Wallet uses legacy HMAC-SHA512 KDF — change password to upgrade to PBKDF2");
    }

    if (timeoutSeconds > 0) {
        unlock_timeout_ = timeoutSeconds;
        unlock_time_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        unlock_timeout_ = 0;
        unlock_time_ = 0;
    }

    // ═══════════════════════════════════════════════════════════════
    // Phase 5b: Load HD wallet master seed into memory
    // ═══════════════════════════════════════════════════════════════

    // Load and decrypt the master seed into memory
    auto seed_opt = loadMasterSeed(passphrase);
    if (seed_opt) {
        // Set master_seed_ from returned value (loadMasterSeed returns but doesn't set member)
        master_seed_ = seed_opt.value();
        WLOG_INFO("✅ HD wallet master seed loaded into memory (" + std::to_string(master_seed_.size()) + " bytes)");

        shielded_incoming_viewing_keys_.clear();
        try {
            constexpr uint32_t kShieldedScanAccounts = 4;
            for (uint32_t acct = 0; acct < kShieldedScanAccounts; ++acct) {
                auto keys = wallet::shielded::DeriveShieldedAccount(
                    master_seed_.data(), master_seed_.size(), acct);
                shielded_incoming_viewing_keys_.push_back(keys.ivk);
            }
            WLOG_INFO("✅ Shielded incoming viewing keys cached for receive scanning (" +
                      std::to_string(shielded_incoming_viewing_keys_.size()) +
                      " account(s))");
        } catch (const std::exception& e) {
            for (auto& ivk : shielded_incoming_viewing_keys_) {
                OPENSSL_cleanse(ivk.data(), ivk.size());
            }
            shielded_incoming_viewing_keys_.clear();
            WLOG_WARN(std::string("Shielded incoming viewing key cache skipped: ") + e.what());
        }
    } else {
        WLOG_WARN("No HD master seed found in database (wallet may not be HD wallet)");
    }

    // ═══════════════════════════════════════════════════════════════
    // V7 PQ master key — load (or generate-on-first-unlock) and cache.
    // See docs/consensus/V7_WALLET_SCHEMA.md §5b. Failure here is
    // logged and swallowed: v5 wallet functionality continues unaffected
    // if the v7 layer hits a snag.
    // ═══════════════════════════════════════════════════════════════
    try {
        std::string v7_hex = getSetting("v7_pq_master_key_encrypted");
        if (v7_hex.empty()) {
            // First unlock since v7 integration landed — generate + persist.
            std::array<uint8_t, 32> fresh{};
            if (RAND_bytes(fresh.data(), static_cast<int>(fresh.size())) != 1) {
                throw std::runtime_error("RAND_bytes failed for v7 PQ master key");
            }
            std::string plaintext(reinterpret_cast<const char*>(fresh.data()), fresh.size());
            std::string encrypted = encryptData(plaintext, encryption_key_);
            std::vector<uint8_t> encrypted_vec(encrypted.begin(), encrypted.end());
            std::string encrypted_hex = util::hex(encrypted_vec);
            setSetting("v7_pq_master_key_encrypted", encrypted_hex);

            std::memcpy(pq_master_key_.data(), fresh.data(), pq_master_key_.size());
            pq_master_key_loaded_ = true;
            OPENSSL_cleanse(fresh.data(),          fresh.size());
            OPENSSL_cleanse(&plaintext[0],         plaintext.size());
            OPENSSL_cleanse(&encrypted[0],         encrypted.size());
            WLOG_INFO("✅ V7 PQ master key generated + persisted (first unlock)");
        } else {
            std::vector<uint8_t> encrypted_bytes;
            if (!util::unhex(v7_hex, encrypted_bytes) || encrypted_bytes.size() < 28) {
                throw std::runtime_error("v7_pq_master_key_encrypted corrupt or too short");
            }
            std::string encrypted(reinterpret_cast<const char*>(encrypted_bytes.data()),
                                  encrypted_bytes.size());
            std::string plaintext = decryptData(encrypted, encryption_key_);
            if (plaintext.size() != pq_master_key_.size()) {
                throw std::runtime_error("v7 PQ master key plaintext length != 32");
            }
            std::memcpy(pq_master_key_.data(), plaintext.data(), pq_master_key_.size());
            pq_master_key_loaded_ = true;
            OPENSSL_cleanse(&plaintext[0], plaintext.size());
            OPENSSL_cleanse(&encrypted[0], encrypted.size());
            WLOG_INFO("✅ V7 PQ master key loaded from wallet settings");
        }
    } catch (const std::exception& e) {
        WLOG_WARN(std::string("V7 PQ master key setup skipped: ") + e.what());
        OPENSSL_cleanse(pq_master_key_.data(), pq_master_key_.size());
        pq_master_key_loaded_ = false;
    }

    // Primary addresses are computed lazily on first getPrimaryAddress() call.

    WLOG_INFO("Wallet unlocked for " +
        (timeoutSeconds > 0 ? std::to_string(timeoutSeconds) + " seconds" : "indefinitely"));
}

// ═══════════════════════════════════════════════════════════════
// V7 post-quantum wallet accessors (spec V7_WALLET_SCHEMA.md §5b)
// ═══════════════════════════════════════════════════════════════

std::optional<std::array<uint8_t, 32>> WalletManager::GetV7PqMasterKey() const {
    if (wallet_locked_ || !pq_master_key_loaded_) {
        return std::nullopt;
    }
    // Return a copy. Caller is responsible for scrubbing.
    std::array<uint8_t, 32> out{};
    std::memcpy(out.data(), pq_master_key_.data(), out.size());
    return out;
}

std::vector<WalletManager::ShieldedIncomingViewingKey>
WalletManager::GetShieldedIncomingViewingKeys() const {
    if (!shielded_incoming_viewing_keys_.empty()) {
        return shielded_incoming_viewing_keys_;
    }

    if (master_seed_.size() != 64) {
        return {};
    }

    std::vector<ShieldedIncomingViewingKey> ivks;
    try {
        constexpr uint32_t kShieldedScanAccounts = 4;
        for (uint32_t acct = 0; acct < kShieldedScanAccounts; ++acct) {
            auto keys = wallet::shielded::DeriveShieldedAccount(
                master_seed_.data(), master_seed_.size(), acct);
            ivks.push_back(keys.ivk);
        }
    } catch (...) {
        ivks.clear();
    }
    return ivks;
}

std::string WalletManager::GetV7P2MRStorePath() const {
    if (current_.empty()) return {};
#ifdef FFI_WALLET_ONLY
    return dataDir_ + "/wallets/v7_p2mr_" + current_ + ".sqlite";
#else
    return (dataDir_ / "wallets" / ("v7_p2mr_" + current_ + ".sqlite")).string();
#endif
}

std::optional<WalletManager::V7Bip32Material>
WalletManager::DeriveV7Bip32Material(uint32_t account,
                                     uint32_t change,
                                     uint32_t address_index) const {
    if (wallet_locked_ || master_seed_.empty()) {
        return std::nullopt;
    }
    try {
        // BIP32Deriver zeroizes its own state on destruction.
        BIP32Deriver deriver(master_seed_.data(), master_seed_.size());
        // Walk m/88'/1448'/account'/change/address_index per
        // V7_WALLET_SCHEMA.md §1. First three levels hardened.
        deriver.deriveHardened(dinero::consensus::PURPOSE_P2MR);
        deriver.deriveHardened(dinero::consensus::DINERO_COIN_TYPE);
        deriver.deriveHardened(account);
        deriver.deriveNormal(change);
        deriver.deriveNormal(address_index);

        V7Bip32Material out{};
        auto priv  = deriver.getPrivateKey();   // 32 bytes
        auto chain = deriver.getChainCode();    // 32 bytes
        std::memcpy(out.private_key.data(), priv.data(),  out.private_key.size());
        std::memcpy(out.chain_code.data(),  chain.data(), out.chain_code.size());
        // deriver dtor scrubs; our locals (priv, chain) are caller-scrubbed
        // in the RPC handler per DerivePQKeypair's contract.
        return out;
    } catch (const std::exception& e) {
        WLOG_WARN(std::string("DeriveV7Bip32Material failed: ") + e.what());
        return std::nullopt;
    }
}

bool WalletManager::isWalletEncrypted() const {
    return getSetting("wallet_encrypted") == "1";
}

bool WalletManager::isWalletLocked() const {
    if (!isWalletEncrypted()) {
        return false;
    }
    
    const_cast<WalletManager*>(this)->checkUnlockTimeout();
    return wallet_locked_;
}

bool WalletManager::setBirthdayHeight(int height) {
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE wallet_meta SET birthday_height = ? WHERE id = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        // Column might not exist yet on older wallets — try adding it
        exec(db_, "ALTER TABLE wallet_meta ADD COLUMN birthday_height INTEGER");
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
    }
    sqlite3_bind_int(stmt, 1, height);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    WLOG_INFO("Birthday height set to " + std::to_string(height));
    return true;
}

int WalletManager::getBirthdayHeight() const {
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT birthday_height FROM wallet_meta WHERE id = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    int height = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        height = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return height;
}

// Balance calculation methods
WalletManager::Balance WalletManager::getBalance(const void* mempool_ptr) const {
    if (!db_ || current_wallet_id_ == -1) {
        WLOG_INFO("getBalance: No database or wallet not open");
        return Balance{};
    }
    
    WLOG_INFO("getBalance: current_wallet_id=" + std::to_string(current_wallet_id_) + ", current_blockchain_height_=" + std::to_string(current_blockchain_height_));
    WLOG_INFO("getBalance: database pointer=" + std::to_string(reinterpret_cast<uintptr_t>(db_)));
    
    Balance balance;
    
    // Query utxos for balance calculation with dynamic coinbase maturity
    // Use positional parameters to avoid name/prefix mismatches
    sqlite3_stmt* stmt;
    const char* sql = R"(
        WITH params(h, w) AS (VALUES (?1, ?2)),
        eligible AS (
          SELECT
            amount,
            is_coinbase,
            ((SELECT h FROM params) - height + 1) AS confs
          FROM utxos
          WHERE is_spent = 0
            AND wallet_id = (SELECT w FROM params)
        )
        SELECT
          COALESCE(SUM(CASE WHEN (confs >= 1)
                                AND (NOT is_coinbase OR confs >= 100)
                            THEN amount END), 0)                                  AS confirmed,
          COALESCE(SUM(CASE WHEN (confs < 1) THEN amount END), 0)                 AS unconfirmed,
          COALESCE(SUM(CASE WHEN is_coinbase AND confs BETWEEN 1 AND 99
                            THEN amount END), 0)                                   AS immature,
          COALESCE(SUM(CASE WHEN (confs >= 1)
                                AND (NOT is_coinbase OR confs >= 100)
                            THEN 1 END), 0)                                        AS spendable_utxo_count,
          COALESCE(SUM(CASE WHEN is_coinbase AND confs BETWEEN 1 AND 99
                            THEN 1 END), 0)                                        AS immature_utxo_count,
          COUNT(*)                                                                 AS total_utxo_count
        FROM eligible
    )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getBalance: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return balance;
    }

    // Assert parameter count to prevent binding bugs
    const int expected_params = 2;
    const int actual_params = sqlite3_bind_parameter_count(stmt);
    if (actual_params != expected_params) {
        WLOG_ERR("getBalance: Expected " + std::to_string(expected_params) +
                              " params, got " + std::to_string(actual_params) +
                              ": " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    // Positional binds: ?1 = current_height, ?2 = active wallet_id
    rc = sqlite3_bind_int(stmt, 1, current_blockchain_height_);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getBalance: bind ?1 failed: " + std::to_string(rc) +
                              " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    rc = sqlite3_bind_int(stmt, 2, current_wallet_id_);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getBalance: bind ?2 failed: " + std::to_string(rc) +
                              " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Get raw values from SQLite
        double confirmed_raw = sqlite3_column_double(stmt, 0);
        double unconfirmed_raw = sqlite3_column_double(stmt, 1);
        double immature_raw = sqlite3_column_double(stmt, 2);
        int spendable_n = sqlite3_column_int(stmt, 3);
        int immature_n = sqlite3_column_int(stmt, 4);
        int total_n = sqlite3_column_int(stmt, 5);

        WLOG_INFO("getBalance row: conf=" + std::to_string(confirmed_raw) + 
                             " unconf=" + std::to_string(unconfirmed_raw) + 
                             " imm=" + std::to_string(immature_raw) + 
                             " sp=" + std::to_string(spendable_n) + 
                             " imm_n=" + std::to_string(immature_n) + 
                             " tot=" + std::to_string(total_n));

        // Convert from base units to DIN (divide by UNA_PER_DIN)
        balance.confirmed = confirmed_raw / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.unconfirmed = unconfirmed_raw / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.immature = immature_raw / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.spendable = balance.confirmed;
        balance.total = balance.confirmed + balance.unconfirmed + balance.immature;
        balance.utxo_count = total_n;
        balance.immature_utxo_count = immature_n;

        WLOG_INFO("getBalance result: confirmed=" + std::to_string(balance.confirmed) + 
                             " unconfirmed=" + std::to_string(balance.unconfirmed) + 
                             " immature=" + std::to_string(balance.immature) + 
                             " total=" + std::to_string(balance.total) + 
                             " utxo_count=" + std::to_string(balance.utxo_count));
    } else {
        WLOG_WARN("getBalance: no row returned (empty eligible set?)");
    }

    sqlite3_finalize(stmt);
    return balance;
}

WalletManager::Balance WalletManager::getAddressBalance(const std::string& address, const void* mempool_ptr) const {
    if (!db_) {
        return Balance{};
    }

    // Prefer scriptPubKey ownership matching when possible.
    // Address text can vary by network prefix while scriptPubKey is canonical.
    if (!address.empty()) {
        sqlite3_stmt* resolve_stmt = nullptr;
        const char* resolve_sql = R"(
            SELECT COALESCE(script_pubkey, '')
            FROM addresses
            WHERE address = ?1
            LIMIT 1
        )";

        int resolve_rc = sqlite3_prepare_v2(db_, resolve_sql, -1, &resolve_stmt, nullptr);
        if (resolve_rc == SQLITE_OK) {
            sqlite3_bind_text(resolve_stmt, 1, address.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(resolve_stmt) == SQLITE_ROW) {
                const char* script_pubkey = reinterpret_cast<const char*>(sqlite3_column_text(resolve_stmt, 0));
                if (script_pubkey && script_pubkey[0] != '\0') {
                    std::string script_pubkey_str(script_pubkey);
                    sqlite3_finalize(resolve_stmt);
                    return getScriptPubKeyBalance(script_pubkey_str, mempool_ptr);
                }
            }
            sqlite3_finalize(resolve_stmt);
        } else if (resolve_stmt) {
            sqlite3_finalize(resolve_stmt);
        }
    }

    Balance balance;
    
    sqlite3_stmt* stmt;
    const char* sql = R"(
        WITH params(h, w) AS (VALUES (?1, ?2)),
        eligible AS (
          SELECT
            amount,
            is_coinbase,
            ((SELECT h FROM params) - height + 1) AS confs
          FROM utxos
          WHERE wallet_id = (SELECT w FROM params)
            AND address = ?3
            AND is_spent = 0
        )
        SELECT
          COALESCE(SUM(CASE WHEN (confs >= 1)
                                AND (NOT is_coinbase OR confs >= 100)
                            THEN amount END), 0)                                  AS confirmed,
          COALESCE(SUM(CASE WHEN (confs < 1) THEN amount END), 0)                 AS unconfirmed,
          COALESCE(SUM(CASE WHEN is_coinbase AND confs BETWEEN 1 AND 99
                            THEN amount END), 0)                                   AS immature,
          COUNT(*)                                                                 AS utxo_count
        FROM eligible
    )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getAddressBalance: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return balance;
    }

    // Assert parameter count to prevent binding bugs
    const int expected_params = 3;
    const int actual_params = sqlite3_bind_parameter_count(stmt);
    if (actual_params != expected_params) {
        WLOG_ERR("getAddressBalance: Expected " + std::to_string(expected_params) + 
                              " params, got " + std::to_string(actual_params) + 
                              ": " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    // Positional binds: ?1 = current_height, ?2 = active wallet_id, ?3 = address
    rc = sqlite3_bind_int(stmt, 1, current_blockchain_height_);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getAddressBalance: bind ?1 failed: " + std::to_string(rc) + 
                              " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    rc = sqlite3_bind_int(stmt, 2, current_wallet_id_);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getAddressBalance: bind ?2 failed: " + std::to_string(rc) +
                              " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    rc = sqlite3_bind_text(stmt, 3, address.c_str(), -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getAddressBalance: bind ?3 failed: " + std::to_string(rc) + 
                              " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Convert from base units to DIN (divide by UNA_PER_DIN)
        balance.confirmed = sqlite3_column_double(stmt, 0) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.unconfirmed = sqlite3_column_double(stmt, 1) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.immature = sqlite3_column_double(stmt, 2) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.utxo_count = sqlite3_column_int(stmt, 3);
        balance.total = balance.confirmed + balance.unconfirmed + balance.immature;
        balance.spendable = balance.confirmed;
    }

    sqlite3_finalize(stmt);
    return balance;
}

WalletManager::Balance WalletManager::getScriptPubKeyBalance(const std::string& script_pubkey, const void* mempool_ptr) const {
    (void)mempool_ptr;
    if (!db_ || script_pubkey.empty()) {
        return Balance{};
    }

    Balance balance;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        WITH params(h, w) AS (VALUES (?1, ?2)),
        eligible AS (
          SELECT
            amount,
            is_coinbase,
            ((SELECT h FROM params) - height + 1) AS confs
          FROM utxos
          WHERE wallet_id = (SELECT w FROM params)
            AND script_pubkey = ?3
            AND is_spent = 0
        )
        SELECT
          COALESCE(SUM(CASE WHEN (confs >= 1)
                                AND (NOT is_coinbase OR confs >= 100)
                            THEN amount END), 0)                                  AS confirmed,
          COALESCE(SUM(CASE WHEN (confs < 1) THEN amount END), 0)                 AS unconfirmed,
          COALESCE(SUM(CASE WHEN is_coinbase AND confs BETWEEN 1 AND 99
                            THEN amount END), 0)                                   AS immature,
          COUNT(*)                                                                 AS utxo_count
        FROM eligible
    )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getScriptPubKeyBalance: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return balance;
    }

    const int expected_params = 3;
    const int actual_params = sqlite3_bind_parameter_count(stmt);
    if (actual_params != expected_params) {
        WLOG_ERR("getScriptPubKeyBalance: Expected " + std::to_string(expected_params) +
                 " params, got " + std::to_string(actual_params) +
                 ": " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    rc = sqlite3_bind_int(stmt, 1, current_blockchain_height_);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getScriptPubKeyBalance: bind ?1 failed: " + std::to_string(rc) +
                 " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    rc = sqlite3_bind_int(stmt, 2, current_wallet_id_);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getScriptPubKeyBalance: bind ?2 failed: " + std::to_string(rc) +
                 " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    rc = sqlite3_bind_text(stmt, 3, script_pubkey.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getScriptPubKeyBalance: bind ?3 failed: " + std::to_string(rc) +
                 " SQL: " + std::string(sqlite3_sql(stmt)));
        sqlite3_finalize(stmt);
        return balance;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance.confirmed = sqlite3_column_double(stmt, 0) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.unconfirmed = sqlite3_column_double(stmt, 1) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.immature = sqlite3_column_double(stmt, 2) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        balance.utxo_count = sqlite3_column_int(stmt, 3);
        balance.total = balance.confirmed + balance.unconfirmed + balance.immature;
        balance.spendable = balance.confirmed;
    }

    sqlite3_finalize(stmt);
    return balance;
}

// Transaction history methods - uses wallet transaction table with real data
std::vector<WalletManager::TransactionInfo> WalletManager::getTransactionHistory(int limit, int offset) const {
    std::vector<TransactionInfo> history;
    
    if (!db_ || current_wallet_id_ == -1) {
        return history;
    }
    
    sqlite3_stmt* stmt;
    // Per-wallet database: no wallet_id filtering needed
    const char* sql = R"(
        SELECT
            t.txid,
            t.address,
            t.amount,
            t.confirmations,
            t.category,
            t.time,
            COALESCE(t.label, COALESCE(a.label, '')) as label,
            t.is_coinbase
        FROM transactions t
        LEFT JOIN addresses a ON t.address = a.address
        ORDER BY t.time DESC, t.confirmations DESC
        LIMIT ? OFFSET ?
    )";

    if (!SqlLog::prepare(&stmt, db_, sql, "tx-history-get-real")) {
        return history;
    }

    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TransactionInfo tx;
        tx.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        tx.address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        tx.amount = sqlite3_column_double(stmt, 2);
        tx.confirmations = sqlite3_column_int(stmt, 3);
        tx.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        tx.time = sqlite3_column_int64(stmt, 5);
        tx.label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        tx.is_coinbase = sqlite3_column_int(stmt, 7) != 0;
        
        history.push_back(tx);
    }
    
    sqlite3_finalize(stmt);
    return history;
}

std::vector<WalletManager::TransactionInfo> WalletManager::getAddressHistory(const std::string& address, int limit) const {
    std::vector<TransactionInfo> history;
    
    if (!db_) {
        return history;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = R"(
        SELECT 
            txid,
            address,
            amount,
            confirmations,
            category,
            time,
            '' as label,
            is_coinbase
        FROM transactions
        WHERE address = ?
        ORDER BY time DESC, confirmations DESC
        LIMIT ?
    )";
    
    if (!SqlLog::prepare(&stmt, db_, sql, "address-history-get")) {
        return history;
    }
    
    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TransactionInfo tx;
        tx.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        tx.address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        tx.amount = sqlite3_column_double(stmt, 2);
        tx.confirmations = sqlite3_column_int(stmt, 3);
        tx.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        tx.time = sqlite3_column_int64(stmt, 5);
        tx.label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        tx.is_coinbase = sqlite3_column_int(stmt, 7) != 0;
        
        history.push_back(tx);
    }
    
    sqlite3_finalize(stmt);
    return history;
}

// Helper: Extract address from scriptPubKey
std::string WalletManager::extractAddressFromScript(const std::vector<uint8_t>& scriptPubKey) const {
    // Handle different script types
    if (scriptPubKey.empty()) {
        return "";
    }

    // Use default mainnet HRP "din" - TODO: make this configurable for testnet/regtest
    const std::string hrp = "din";

    // P2WPKH: OP_0 PUSH20 <20-byte-pubkey-hash>
    if (scriptPubKey.size() == 22 &&
        scriptPubKey[0] == 0x00 &&
        scriptPubKey[1] == 0x14) {
        // Extract 20-byte pubkey hash
        std::vector<uint8_t> pubkey_hash(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Encode as Bech32 P2WPKH address (witness v0, 20 bytes)
        return bech32::Encode(hrp, 0, pubkey_hash);
    }

    // P2WSH: OP_0 PUSH32 <32-byte-script-hash>
    if (scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x00 &&
        scriptPubKey[1] == 0x20) {
        // Extract 32-byte script hash
        std::vector<uint8_t> script_hash(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Encode as Bech32 P2WSH address (witness v0, 32 bytes)
        return bech32::Encode(hrp, 0, script_hash);
    }

    // P2TR (Taproot): OP_1 PUSH32 <32-byte-witness-program>
    if (scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x51 &&
        scriptPubKey[1] == 0x20) {
        // Extract 32-byte witness program
        std::vector<uint8_t> witness_program(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Encode as Bech32m Taproot address (witness v1, 32 bytes)
        return bech32::Encode(hrp, 1, witness_program);
    }

    // Unknown or legacy script type - return empty string
    // Could add P2PKH, P2SH support here if needed
    return "";
}

// Encryption helper methods
std::string WalletManager::deriveKey(const std::string& passphrase, const std::string& salt) const {
    // PBKDF2-HMAC-SHA512 with 210 000 iterations (OWASP 2023 minimum for SHA-512)
    constexpr uint32_t ITERATIONS = 210000;
    uint8_t derived[64];
    dinero::crypto::PBKDF2_HMAC_SHA512(
        reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size(),
        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
        ITERATIONS,
        derived, 32
    );

    std::string result(reinterpret_cast<char*>(derived), 32);
    OPENSSL_cleanse(derived, sizeof(derived));
    return result;
}

std::string WalletManager::deriveKeyLegacy(const std::string& passphrase, const std::string& salt) const {
    // Pre-v0.4.0 key derivation: single-pass HMAC-SHA512
    // Kept for backward compatibility with wallets encrypted before the PBKDF2 migration
    uint8_t hash[64];
    hmac_sha512(reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size(),
                hash);
    std::string result(reinterpret_cast<char*>(hash), 32);
    OPENSSL_cleanse(hash, sizeof(hash));
    return result;
}

std::string WalletManager::encryptData(const std::string& data, const std::string& key) const {
    // Use AES-256-GCM encryption with random nonce

    // Validate key size (must be 32 bytes)
    if (key.size() != 32) {
        throw std::runtime_error("Encryption key must be exactly 32 bytes");
    }

    // Generate random 12-byte nonce for AES-GCM
    std::vector<uint8_t> nonce(12);
    if (RAND_bytes(nonce.data(), nonce.size()) != 1) {
        throw std::runtime_error("Failed to generate random nonce");
    }

    // Convert key from string to array
    std::array<uint8_t, 32> key_array;
    std::memcpy(key_array.data(), key.data(), 32);

    // Convert plaintext to vector
    std::vector<uint8_t> plaintext(data.begin(), data.end());

    // Encrypt using AES-256-GCM
    std::vector<uint8_t> ciphertext = crypto::encryptAesGcm(plaintext, key_array, nonce);

    // Format: nonce (12 bytes) + ciphertext + tag (16 bytes)
    // Prepend nonce to ciphertext so we can extract it during decryption
    std::vector<uint8_t> result;
    result.reserve(nonce.size() + ciphertext.size());
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.end());

    // Convert to string
    return std::string(result.begin(), result.end());
}

std::string WalletManager::decryptData(const std::string& encryptedData, const std::string& key) const {
    // Use AES-256-GCM decryption with nonce extracted from encrypted data

    // Validate key size (must be 32 bytes)
    if (key.size() != 32) {
        throw std::runtime_error("Decryption key must be exactly 32 bytes");
    }

    // Validate encrypted data size (must be at least nonce + tag = 12 + 16 = 28 bytes)
    if (encryptedData.size() < 28) {
        throw std::runtime_error("Encrypted data too short (corrupted)");
    }

    // Extract nonce (first 12 bytes)
    std::vector<uint8_t> nonce(encryptedData.begin(), encryptedData.begin() + 12);

    // Extract ciphertext + tag (remaining bytes)
    std::vector<uint8_t> ciphertext(encryptedData.begin() + 12, encryptedData.end());

    // Convert key from string to array
    std::array<uint8_t, 32> key_array;
    std::memcpy(key_array.data(), key.data(), 32);

    // Decrypt using AES-256-GCM
    std::vector<uint8_t> plaintext = crypto::decryptAesGcm(ciphertext, key_array, nonce);

    // Convert to string
    return std::string(plaintext.begin(), plaintext.end());
}

void WalletManager::checkUnlockTimeout() {
    if (unlock_timeout_ > 0 && unlock_time_ > 0) {
        int64_t currentTime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        if (currentTime - unlock_time_ >= unlock_timeout_) {
            wallet_locked_ = true;
            secureClearString(encryption_key_);
            clearPrivateKeyCache();
            secureClearBytes(master_seed_);
            unlock_timeout_ = 0;
            unlock_time_ = 0;
            WLOG_INFO("Wallet automatically locked due to timeout");
        }
    }
}

// UTXO management for PSBT creation
std::vector<WalletManager::WalletUTXO> WalletManager::listUnspentUTXOs(int min_confirmations,
                                                                       int max_confirmations,
                                                                       const Mempool* mempool) const {
    std::vector<WalletManager::WalletUTXO> utxos;
    
    if (!db_) {
        return utxos;
    }
    
    // Query UTXOs with dynamic maturity computation (no stored is_mature dependency)
    // JOIN with address_derivation_paths to get BIP32 derivation path for signing.
    // FALLBACK 1: If address_derivation_paths is empty (legacy wallets), construct the
    // derivation path from the addresses table (type, account, change, idx).
    // FALLBACK 2 (Phase 10): v7 P2MR addresses live in watch_scripts, not
    // addresses/address_derivation_paths. LEFT JOIN watch_scripts as a third
    // COALESCE source so P2MR UTXOs carry a non-empty derivation_path, which
    // wallet.sendtoaddress requires to admit them into the coin-selector input set.
    // watch_scripts stores the scriptPubKey as a BLOB; utxos.script_pubkey is the
    // lower-hex string, so we compare via lower(hex(ws.script_pubkey)).
    // NOTE: JOIN on script_pubkey (not address) because addresses may use different
    // network prefixes (din1/rdin1) while script_pubkey is network-independent
    sqlite3_stmt* stmt;
    const char* sql = R"(
        SELECT u.txid, u.vout, u.address, u.amount, u.script_pubkey, u.height, u.is_coinbase, u.is_spent,
               COALESCE(adp.derivation_path,
                        CASE WHEN a.type IS NOT NULL THEN
                            'm/' || CASE WHEN a.type = 'p2tr' THEN '86' ELSE '84' END ||
                            '''/' || ?1 || '''/' || COALESCE(a.account, 0) || '''/' ||
                            COALESCE(a.change, 0) || '/' || COALESCE(a.idx, 0)
                        END,
                        ws.path)
               AS derivation_path
        FROM utxos u
        LEFT JOIN address_derivation_paths adp ON u.script_pubkey = adp.script_pubkey
        LEFT JOIN addresses a ON u.script_pubkey = a.script_pubkey
        LEFT JOIN watch_scripts ws ON lower(hex(ws.script_pubkey)) = u.script_pubkey
        WHERE u.is_spent = 0
          AND u.wallet_id = ?2
        ORDER BY u.amount DESC
    )";

    if (!SqlLog::prepare(&stmt, db_, sql, "list-utxos-with-derivation-path")) {
        return utxos;
    }

    // Bind canonical Dinero coin type for fallback derivation-path reconstruction.
    if (sqlite3_bind_int(stmt, 1, static_cast<int>(dinero::consensus::DINERO_COIN_TYPE)) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return utxos;
    }

    if (sqlite3_bind_int(stmt, 2, current_wallet_id_) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return utxos;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WalletManager::WalletUTXO utxo;

        // Extract UTXO data
        const char* txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        uint32_t vout = sqlite3_column_int(stmt, 1);
        const char* address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        int64_t amount_una = sqlite3_column_int64(stmt, 3);
        const char* script_pubkey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        uint32_t height = sqlite3_column_int(stmt, 5);
        bool is_coinbase = sqlite3_column_int(stmt, 6) != 0;
        bool is_spent = sqlite3_column_int(stmt, 7) != 0;
        const char* derivation_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));

        // SEATBELT: Validate critical fields before use
        if (!txid || strlen(txid) == 0) {
            logCorruptRow("utxos", "txid", "NULL or empty txid");
            continue;  // Skip this row, continue with others
        }
        if (!address || strlen(address) == 0) {
            logCorruptRow("utxos", "address", "NULL or empty address");
            continue;  // Skip this row, continue with others
        }

        utxo.txid = txid;
        utxo.vout = vout;
        utxo.amount_una = amount_una;
        utxo.amount_din = static_cast<double>(amount_una) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        utxo.address = address;
        utxo.height = height;
        utxo.is_coinbase = is_coinbase;
        utxo.is_spent = is_spent;
        utxo.script_pubkey = script_pubkey ? script_pubkey : "";
        utxo.derivation_path = derivation_path ? derivation_path : "";
        utxo.label = "";

        // Calculate confirmations from current blockchain height
        utxo.confirmations = (current_blockchain_height_ > height) ?
                            (current_blockchain_height_ - height + 1) : 0;

        // Compute maturity dynamically (no stored boolean dependency)
        const uint32_t COINBASE_MATURITY = 100;
        utxo.is_mature = !is_coinbase || (utxo.confirmations >= COINBASE_MATURITY); // >= 100 for coinbase

        // Bug Fix 1: Skip immature coinbase outputs — they cannot be spent yet.
        // A coinbase UTXO with < 100 confirmations will be rejected by consensus.
        if (is_coinbase && !utxo.is_mature) {
            continue;
        }

        // Check if spendable (dynamic maturity + confirmation range)
        utxo.spendable = utxo.is_mature &&
                        (utxo.confirmations >= min_confirmations) &&
                        (utxo.confirmations <= max_confirmations);

        // Bug Fix 2: Validate UTXO against the chain UTXO index to exclude
        // stale/spent entries that the wallet DB has not yet marked as spent.
        // This catches transparent UTXOs consumed by ring/unshield transactions
        // where the wallet index was not updated (e.g. fee inputs for CT spends).
        if (utxo_index_) {
            try {
                TxId chain_txid(uint256::FromHexUnsafe(utxo.txid));
                auto chain_utxo = utxo_index_->GetUTXO(chain_txid, utxo.vout);
                if (!chain_utxo.has_value()) {
                    // A snapshot-anchored coin (recorded from the AssumeUTXO
                    // snapshot) is committed in the utreexo accumulator and is
                    // NEVER enumerable in the live utxo_index_. Do NOT infer it
                    // spent — this read-path was wiping fast-synced wallets'
                    // pre-snapshot balances. Genuine spends above the base still
                    // arrive via the input-gated block-connect paths.
                    bool anchored = false;
                    {
                        sqlite3_stmt* astmt = nullptr;
                        const char* asql = "SELECT snapshot_anchored FROM utxos "
                                           "WHERE txid = ? AND vout = ? AND wallet_id = ? LIMIT 1";
                        if (sqlite3_prepare_v2(db_, asql, -1, &astmt, nullptr) == SQLITE_OK) {
                            sqlite3_bind_text(astmt, 1, utxo.txid.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int(astmt, 2, static_cast<int>(utxo.vout));
                            sqlite3_bind_int(astmt, 3, current_wallet_id_);
                            if (sqlite3_step(astmt) == SQLITE_ROW) {
                                anchored = sqlite3_column_int(astmt, 0) != 0;
                            }
                            sqlite3_finalize(astmt);
                        }
                        // If the column doesn't exist (legacy wallet), prepare
                        // fails and anchored stays false → original behavior.
                    }
                    if (!anchored) {
                        // Cross-store mismatch: the coin is in the wallet DB but
                        // absent from the in-memory chain UTXO index, and it is NOT
                        // a snapshot-anchored coin.
                        //
                        // SECURITY (fund-loss fix, audit Fix 2): a const READ method
                        // must NEVER mutate fund state. The previous code persisted
                        // `UPDATE utxos SET is_spent=1` here, which irreversibly
                        // zeroed legitimate balances on the first listunspent. We now
                        // treat the mismatch as non-destructive: skip the coin from
                        // THIS result set (so coin-selection won't try to spend an
                        // output the chainstate can't currently see) but leave the DB
                        // untouched, so the coin reappears once utxo_index_ is
                        // populated.
                        logCorruptRow("utxos", "chain-mismatch",
                                      "wallet UTXO absent from chain index; skipping "
                                      "(read-only, no state mutation)");
                        continue;
                    }
                    // anchored: valid snapshot coin — keep it (transparent, not CT).
                } else {
                    // Propagate CT flag from chain UTXO set into wallet UTXO view
                    utxo.is_confidential = chain_utxo->is_confidential;
                }
            } catch (const std::exception&) {
                // If the txid is malformed we can't validate — skip to be safe.
                logCorruptRow("utxos", "txid", "failed to parse txid for chain UTXO validation");
                continue;
            }
        }

        if (mempool) {
            try {
                const OutPoint outpoint(TxId(uint256::FromHexUnsafe(utxo.txid)), utxo.vout);
                if (mempool->isOutputSpentInMempool(outpoint)) {
                    continue;
                }
            } catch (const std::exception&) {
                logCorruptRow("utxos", "txid", "failed to parse txid for mempool spend filter");
                continue;
            }
        }

        // Only include UTXOs with positive amounts
        if (utxo.amount_una > 0) {
            utxos.push_back(utxo);
        }
    }
    
    sqlite3_finalize(stmt);
    return utxos;
}

std::vector<WalletManager::WalletUTXO> WalletManager::getUTXOsForAddress(const std::string& address, int min_confirmations) const {
    std::vector<WalletManager::WalletUTXO> utxos;
    
    if (!db_ || address.empty()) {
        return utxos;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = R"(
        SELECT txid, address, amount, confirmations, category, label, time, is_coinbase
        FROM transactions 
        WHERE address = ? 
        AND category IN ('generate', 'mining', 'coinbase', 'receive')
        AND confirmations >= ?
        AND wallet_id = ?
        ORDER BY confirmations DESC, amount DESC
    )";
    
    if (!SqlLog::prepare(&stmt, db_, sql, "get-address-utxos-with-maturity")) {
        return utxos;
    }
    
    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, min_confirmations);
    sqlite3_bind_int(stmt, 3, current_wallet_id_);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WalletManager::WalletUTXO utxo;
        
        const char* txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* addr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        double amount = sqlite3_column_double(stmt, 2);
        int confirmations = sqlite3_column_int(stmt, 3);
        const char* category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        bool is_coinbase = sqlite3_column_int(stmt, 7) != 0;
        
        if (txid && addr && category) {
            utxo.txid = txid;
            utxo.vout = 0;
            utxo.amount_din = amount;
            utxo.amount_una = static_cast<uint64_t>(amount * 1000000);
            utxo.address = addr;
            utxo.confirmations = confirmations;
            utxo.height = current_blockchain_height_ > confirmations ? 
                         current_blockchain_height_ - confirmations + 1 : 0;
            utxo.is_coinbase = is_coinbase;
            utxo.label = label ? label : "";
            
            // Check coinbase maturity
            if (is_coinbase) {
                utxo.is_mature = dinero::CoinbaseMaturity::isCoinbaseMature(utxo.height, current_blockchain_height_);
                utxo.spendable = confirmations >= min_confirmations && utxo.is_mature && amount > 0;
            } else {
                utxo.is_mature = true;
                utxo.spendable = confirmations >= min_confirmations && amount > 0;
            }
            
            if (utxo.amount_una > 0) {
                utxos.push_back(utxo);
            }
        }
    }
    
    sqlite3_finalize(stmt);
    return utxos;
}

// ═══════════════════════════════════════════════════════════════
// Phase 35: UTXO Locking (wallet.lockunspent)
// ═══════════════════════════════════════════════════════════════

bool WalletManager::lockUTXO(const std::string& txid, uint32_t vout) {
    std::string outpoint = txid + ":" + std::to_string(vout);
    locked_utxos_.insert(outpoint);
    return true;
}

bool WalletManager::unlockUTXO(const std::string& txid, uint32_t vout) {
    std::string outpoint = txid + ":" + std::to_string(vout);
    auto it = locked_utxos_.find(outpoint);
    if (it != locked_utxos_.end()) {
        locked_utxos_.erase(it);
        return true;
    }
    return false;  // Was not locked
}

bool WalletManager::isUTXOLocked(const std::string& txid, uint32_t vout) const {
    std::string outpoint = txid + ":" + std::to_string(vout);
    return locked_utxos_.find(outpoint) != locked_utxos_.end();
}

std::vector<std::string> WalletManager::getLockedUTXOs() const {
    return std::vector<std::string>(locked_utxos_.begin(), locked_utxos_.end());
}

size_t WalletManager::unlockAllUTXOs() {
    size_t count = locked_utxos_.size();
    locked_utxos_.clear();
    return count;
}

double WalletManager::getLockedBalance() const {
    double locked_balance = 0.0;

    if (!db_) {
        return 0.0;
    }

    // Query all UTXOs and sum amounts for locked ones
    for (const std::string& outpoint : locked_utxos_) {
        // Parse txid:vout
        size_t colon_pos = outpoint.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string txid = outpoint.substr(0, colon_pos);
        uint32_t vout = std::stoul(outpoint.substr(colon_pos + 1));

        // Query UTXO amount
        sqlite3_stmt* stmt;
        const char* sql = "SELECT amount FROM utxos WHERE txid = ? AND vout = ? AND is_spent = 0";

        if (!SqlLog::prepare(&stmt, db_, sql, "get-locked-utxo-amount")) {
            continue;
        }

        sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, vout);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t amount_una = sqlite3_column_int64(stmt, 0);
            locked_balance += static_cast<double>(amount_una) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        }

        sqlite3_finalize(stmt);
    }

    return locked_balance;
}

// Phase 35.4: Transaction Abandonment
bool WalletManager::abandonTransaction(const std::string& txid) {
    if (!db_) {
        return false;
    }

    // Check if transaction exists in wallet
    sqlite3_stmt* stmt;
    const char* check_sql = "SELECT confirmations FROM transactions WHERE txid = ?";

    if (!SqlLog::prepare(&stmt, db_, check_sql, "check-tx-for-abandon")) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;  // Transaction not found
    }

    int confirmations = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // Cannot abandon confirmed transactions
    if (confirmations > 0) {
        return false;
    }

    // Mark as abandoned
    abandoned_transactions_.insert(txid);

    return true;
}

bool WalletManager::isTransactionAbandoned(const std::string& txid) const {
    return abandoned_transactions_.find(txid) != abandoned_transactions_.end();
}

WalletManager::AbandonmentInfo WalletManager::getAbandonmentInfo(const std::string& txid) const {
    AbandonmentInfo info;
    info.success = false;
    info.inputs_returned = 0;
    info.amount_returned = 0.0;

    if (!db_) {
        info.error = "Wallet not loaded";
        return info;
    }

    // Check if transaction exists
    sqlite3_stmt* stmt;
    const char* check_sql = "SELECT confirmations FROM transactions WHERE txid = ?";

    if (!SqlLog::prepare(&stmt, db_, check_sql, "check-tx-exists")) {
        info.error = "Database error";
        return info;
    }

    sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        info.error = "Transaction not found in wallet";
        return info;
    }

    int confirmations = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (confirmations > 0) {
        info.error = "Cannot abandon confirmed transaction";
        return info;
    }

    // Note: The wallet DB doesn't track which transaction spent which UTXO,
    // so we can't calculate exact inputs_returned. The abandonment still works
    // (inputs become spendable again when the transaction is no longer considered),
    // we just don't report detailed statistics.
    info.inputs_returned = 0;
    info.amount_returned = 0.0;
    info.success = true;
    info.error = "";

    return info;
}

// Get all addresses belonging to the current wallet
std::vector<std::string> WalletManager::getWalletAddresses() const {
    std::vector<std::string> addresses;

    if (!db_) {
        return addresses;
    }

    sqlite3_stmt* stmt;
    // Per-wallet database schema - no wallet_id column needed
    const char* sql = "SELECT address FROM addresses";

    if (!SqlLog::prepare(&stmt, db_, sql, "get-wallet-addresses")) {
        return addresses;
    }

    // No binding needed - query all addresses in this wallet's database

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (address) {
            addresses.push_back(address);
        }
    }

    sqlite3_finalize(stmt);
    return addresses;
}

// Add a transaction to the wallet database
// Phase 36: Added height parameter for reorg handling
bool WalletManager::addTransaction(const std::string& txid, const std::string& address, double amount,
                                 const std::string& category, bool is_coinbase,
                                 const std::string& label, int64_t time, uint32_t height) {
    if (!db_ || current_wallet_id_ == -1) {
        return false;
    }

    if (time == 0) {
        time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Confirmations:
    // - height == 0 => unconfirmed (mempool), keep at 0
    // - height > 0  => derive from current tip when available
    int confirmations = 0;
    if (height > 0) {
        confirmations = (current_blockchain_height_ >= height)
            ? static_cast<int>(current_blockchain_height_ - height + 1)
            : 1;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        INSERT OR REPLACE INTO transactions
        (wallet_id, txid, address, amount, confirmations, category, label, time, is_coinbase, height)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    const bool prepared = SqlLog::prepare(&stmt, db_, sql, "add-transaction");
    if (!prepared) {
        return false;
    }

    int bind_index = 1;
    sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
    sqlite3_bind_text(stmt, bind_index++, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, bind_index++, amount);
    sqlite3_bind_int(stmt, bind_index++, confirmations);
    sqlite3_bind_text(stmt, bind_index++, category.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, bind_index++, label.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, bind_index++, time);
    sqlite3_bind_int(stmt, bind_index++, is_coinbase ? 1 : 0);
    sqlite3_bind_int(stmt, bind_index++, static_cast<int>(height));
    
    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (result == SQLITE_DONE) {
        WLOG_INFO("Added transaction to wallet: " + txid + " (" + category + ") " +
                             std::to_string(amount) + " DIN to " + address);
        return true;
    } else {
        WLOG_ERR("Failed to add transaction to wallet: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
}

bool WalletManager::confirmTransaction(const std::string& txid, uint32_t height) {
    if (!db_ || current_wallet_id_ == -1 || txid.empty() || height == 0) {
        return false;
    }

    if (!columnExists(db_, "transactions", "height") ||
        !columnExists(db_, "transactions", "confirmations")) {
        WLOG_WARN("confirmTransaction: transactions table lacks height/confirmations columns");
        return false;
    }

    const uint32_t tip_height = std::max(current_blockchain_height_, height);
    const int confirmations = static_cast<int>(tip_height - height + 1);
    const bool has_wallet_id = columnExists(db_, "transactions", "wallet_id");

    sqlite3_stmt* stmt = nullptr;
    const char* sql_with_wallet_id = R"(
        UPDATE transactions
        SET height = ?, confirmations = ?
        WHERE wallet_id = ? AND txid = ?
    )";
    const char* sql_without_wallet_id = R"(
        UPDATE transactions
        SET height = ?, confirmations = ?
        WHERE txid = ?
    )";

    const bool prepared = has_wallet_id
        ? SqlLog::prepare(&stmt, db_, sql_with_wallet_id, "confirm-transaction(with-wallet-id)")
        : SqlLog::prepare(&stmt, db_, sql_without_wallet_id, "confirm-transaction(no-wallet-id)");
    if (!prepared) {
        return false;
    }

    int bind_index = 1;
    sqlite3_bind_int(stmt, bind_index++, static_cast<int>(height));
    sqlite3_bind_int(stmt, bind_index++, confirmations);
    if (has_wallet_id) {
        sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
    }
    sqlite3_bind_text(stmt, bind_index++, txid.c_str(), -1, SQLITE_STATIC);

    const int result = sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (result != SQLITE_DONE) {
        WLOG_ERR("Failed to confirm transaction " + txid + ": " +
                 std::string(sqlite3_errmsg(db_)));
        return false;
    }

    if (changes > 0) {
        WLOG_INFO("Confirmed wallet transaction " + txid +
                  " at height " + std::to_string(height) +
                  " (" + std::to_string(confirmations) + " confirmations)");
        return true;
    }

    return false;
}

// Phase 36: Remove transactions from orphaned blocks during reorg
bool WalletManager::removeTransactionsAtHeight(uint32_t height) {
    if (!db_ || current_wallet_id_ == -1) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql_with_wallet_id = "DELETE FROM transactions WHERE wallet_id = ? AND height = ?";
    const char* sql_without_wallet_id = "DELETE FROM transactions WHERE height = ?";

    const bool has_wallet_id = columnExists(db_, "transactions", "wallet_id");
    const bool prepared = has_wallet_id
        ? SqlLog::prepare(&stmt, db_, sql_with_wallet_id, "remove-transactions-at-height(with-wallet-id)")
        : SqlLog::prepare(&stmt, db_, sql_without_wallet_id, "remove-transactions-at-height(no-wallet-id)");
    if (!prepared) {
        return false;
    }
    if (!has_wallet_id) {
        WLOG_WARN("removeTransactionsAtHeight: using legacy transactions schema without wallet_id");
    }

    int bind_index = 1;
    if (has_wallet_id) {
        sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
    }
    sqlite3_bind_int(stmt, bind_index++, static_cast<int>(height));

    int result = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (result == SQLITE_DONE) {
        WLOG_INFO("Removed " + std::to_string(changes) + " transactions at height " + std::to_string(height));
        return true;
    } else {
        WLOG_ERR("Failed to remove transactions at height " + std::to_string(height) + ": " +
                std::string(sqlite3_errmsg(db_)));
        return false;
    }
}

// Analyze a raw transaction to determine its impact on wallet addresses
bool WalletManager::analyzeTransaction(const char* raw_hex, const std::vector<std::string>& wallet_addresses, 
                                     TransactionInfo& tx, uint32_t height) const {
    if (!raw_hex) return false;
    
    // For now, implement a simplified transaction analysis
    // In production, this would parse the raw transaction hex
    
    // Check if this is a coinbase transaction (simplified detection)
    std::string hex_str(raw_hex);
    bool is_coinbase = (height == 0) || (hex_str.find("0000000000000000000000000000000000000000000000000000000000000000") != std::string::npos);
    
    tx.is_coinbase = is_coinbase;
    
    if (is_coinbase) {
        // Mining reward transaction
        tx.category = "generate";
        tx.amount = calculateMiningReward(height);
        
        // For mining rewards, use the first wallet address as the recipient
        if (!wallet_addresses.empty()) {
            tx.address = wallet_addresses[0];
            tx.label = "Mining reward";
            return true;
        }
    } else {
        // Regular transaction - would need full parsing to determine inputs/outputs
        // For now, assume it's a receive transaction
        tx.category = "receive";
        tx.amount = 1.0; // Placeholder - would parse actual amount from outputs
        
        if (!wallet_addresses.empty()) {
            tx.address = wallet_addresses[0];
            tx.label = "Received";
            return true;
        }
    }
    
    return false;
}

// Calculate mining reward based on height (simplified)
double WalletManager::calculateMiningReward(uint32_t height) const {
    if (height == 0) {
        return 100000.0; // Genesis block reward
    } else if (height == 1) {
        return 2000000.0; // Developer fund
    } else {
        // CPU-friendly phase: 99 DIN per block
        // This is simplified - in production would use the actual DineroAlgorithm
        return 99.0;
    }
}

// Address generation methods
std::string WalletManager::getNewAddress(const std::string& label, const std::string& address_type) {
    if (!hasActiveWallet()) {
        WLOG_ERR("No active wallet for address generation");
        return "";
    }

    if (current_wallet_id_ == -1) {
        WLOG_ERR("No wallet ID set for address generation");
        return "";
    }

    // ═══════════════════════════════════════════════════════════════
    // Phase 3C: Descriptor-Based Address Generation
    // ═══════════════════════════════════════════════════════════════
    // Check if there's an active descriptor for receive addresses.
    // Wallet is taproot-only; descriptor policy can only confirm taproot.
    std::string effective_address_type = "taproot";

    // Legacy/segwit requests are explicitly ignored in taproot-only mode.
    if (!address_type.empty()) {
        std::string requested;
        requested.reserve(address_type.size());
        for (char c : address_type) {
            requested.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (requested != "taproot" && requested != "p2tr" && requested != "bech32m" &&
            requested != "witness_v1_taproot") {
            WLOG_WARN("Taproot-only mode: ignoring non-taproot address_type request '" + address_type + "'");
        }
    }

    if (descriptor_store_) {
        // Get all active descriptors
        std::vector<din::DescriptorRecord> active_descriptors = descriptor_store_->listDescriptors(true);  // active_only=true

        // Find the receive (is_change=false) descriptor
        for (const auto& desc : active_descriptors) {
            if (!desc.is_change) {
                if (desc.policy == "BIP86") {
                    effective_address_type = "taproot";
                    WLOG_INFO("[Descriptor] Using active BIP86 receive descriptor (id=" + std::to_string(desc.id) +
                             ", account=" + std::to_string(desc.account) + ") → Taproot address generation");
                } else if (desc.policy == "BIP84") {
                    WLOG_WARN("[Descriptor] Active BIP84 receive descriptor (id=" + std::to_string(desc.id) +
                             ", account=" + std::to_string(desc.account) + ") ignored in taproot-only mode");
                } else {
                    WLOG_WARN("[Descriptor] Unknown policy '" + desc.policy + "' (id=" + std::to_string(desc.id) +
                             ") → keeping Taproot-only generation");
                }
                break;  // Use first active receive descriptor
            }
        }

        // Log if no active descriptor was found
        if (effective_address_type == "taproot" && !active_descriptors.empty()) {
            WLOG_INFO("[Descriptor] No active BIP86 receive descriptor found → keeping Taproot-only generation");
        }
    } else {
        // No descriptor store configured
        WLOG_DEBUG("[Descriptor] DescriptorStore not configured → using Taproot-only generation");
    }

    // ═══════════════════════════════════════════════════════════════
    // Phase 6D: Generate address using BIP84 (legacy) or BIP86 (taproot)
    // ═══════════════════════════════════════════════════════════════

    // Check if master seed is available. For unencrypted wallets, recover defensively.
    if (master_seed_.empty() && !wallet_locked_) {
        auto seed_opt = loadMasterSeed("");
        if (seed_opt.has_value()) {
            master_seed_ = std::move(seed_opt.value());
            WLOG_INFO("Recovered master seed in-memory for address generation");
        }
    }
    if (master_seed_.empty()) {
        WLOG_ERR("HD wallet not initialized - master seed not available");
        return "";
    }

    // Validate address_type
    if (effective_address_type != "taproot") {
        WLOG_ERR("Invalid address_type: " + effective_address_type);
        return "";
    }

    try {
        std::string address;
        std::string script_pubkey;
        std::vector<uint8_t> script_bytes;
        int next_index = getNextAddressIndex(0, 0);

        // ═══ Week 1 Day 2: KeyID storage (descriptor wallet foundation) ═══
        // Declare KeyID variables here so they're accessible at database INSERT
        std::optional<wallet::KeyID> key_id;           // Primary KeyID
        std::optional<wallet::KeyID> internal_key_id;  // Taproot: internal key
        std::optional<wallet::KeyID> output_key_id;    // Taproot: tweaked output key

        if (effective_address_type == "taproot") {
            // ═══ BIP86 Taproot Address Generation ═══
            // Derive BIP86 key path: m/86'/1448'/0'/0/index
            WLOG_INFO("[Taproot] Starting Taproot address generation for index " + std::to_string(next_index));

            auto master_key = dinero::crypto::HDKeychain::fromSeed(master_seed_);
            // Derive BIP86 key: m/86'/1448'/0'/0/index
            auto account_key = master_key.derive(86 | 0x80000000);  // 86'
            auto coin_key = account_key.derive(dinero::consensus::DINERO_COIN_TYPE | 0x80000000);  // coin_type'
            auto account0_key = coin_key.derive(0 | 0x80000000);  // 0'
            auto external_key = account0_key.derive(0);  // 0 (receive chain)
            auto derived_key = external_key.derive(static_cast<uint32_t>(next_index));

            // Get 33-byte compressed public key (internal key)
            auto pubkey = derived_key.getPublicKey();
            if (pubkey.size() != 33) {
                WLOG_ERR("[Taproot] Derived pubkey is not 33 bytes");
                return "";
            }

            // Create x-only internal pubkey (drop 0x02/0x03 prefix)
            std::vector<uint8_t> xonly_pubkey(pubkey.begin() + 1, pubkey.end());

            // Compute BIP341 tweaked output key
            std::array<uint8_t, 32> output_key{};
            if (!ComputeTaprootOutputKey(xonly_pubkey, output_key, logger_)) {
                return "";
            }

            // Create Taproot address (Bech32m encoding with witness version 1)
            std::string hrp = dinero::HrpForActiveNetworkRef();
            if (hrp.empty()) {
                hrp = "din";
            }

            std::vector<uint8_t> witness_program(output_key.begin(), output_key.end());
            address = bech32::Encode(hrp, 1, witness_program, bech32::Encoding::BECH32M);

            if (address.empty()) {
                WLOG_ERR("[Taproot] Failed to encode Taproot address - Bech32m encoding returned empty");
                return "";
            }

            // Build P2TR scriptPubKey: OP_1 (0x51) PUSH32 (0x20) <32-byte tweaked key>
            script_bytes.push_back(0x51);  // OP_1
            script_bytes.push_back(0x20);  // Push 32 bytes
            script_bytes.insert(script_bytes.end(), output_key.begin(), output_key.end());
            script_pubkey = "5120" + bytesToHex(output_key.data(), output_key.size());

            // ═══ Week 1 Day 2: Compute KeyIDs for descriptor wallet ═══
            // For Taproot, we need THREE KeyIDs:
            // 1. internal_key_id: from x-only internal key (before TapTweak)
            // 2. output_key_id: from tweaked output key (scriptPubKey)
            // 3. key_id: primary identifier (same as internal_key_id)

            std::array<uint8_t, 32> xonly_internal;
            std::copy(xonly_pubkey.begin(), xonly_pubkey.end(), xonly_internal.begin());

            internal_key_id = wallet::ComputeKeyIDFromXOnly(xonly_internal);
            output_key_id = wallet::ComputeKeyIDFromXOnly(output_key);
            key_id = internal_key_id.value();  // Primary ID is internal key

            WLOG_INFO("[Taproot] Computed KeyIDs:");
            WLOG_INFO("  internal_key_id: " + wallet::KeyIDToHex(internal_key_id.value()));
            WLOG_INFO("  output_key_id:   " + wallet::KeyIDToHex(output_key_id.value()));
            WLOG_INFO("[Taproot] Successfully generated Taproot address: " + address);
        } else {
            // ═══ BIP84 Legacy (P2WPKH) Address Generation ═══
            auto master_key = dinero::crypto::HDKeychain::fromSeed(master_seed_);
            auto derived_key = dinero::crypto::HDKeychain::deriveBIP84(
                master_key,
                dinero::consensus::DINERO_COIN_TYPE,
                0,  // account 0
                0,  // external chain (receive addresses)
                static_cast<uint32_t>(next_index)
            );

            // Get address and public key
            address = derived_key.getAddress(dinero::HrpForActiveNetworkRef());
            auto hash160 = derived_key.getHash160();

            // Create scriptPubKey (P2WPKH: 0014 + 20-byte hash)
            script_pubkey = "0014" + bytesToHex(hash160.data(), hash160.size());

            // ═══ Week 1 Day 2: Compute KeyID for BIP84 ═══
            // For P2WPKH, we only need one KeyID (from compressed pubkey)
            // internal_key_id and output_key_id remain NULL for non-Taproot
            auto pubkey_33 = derived_key.getPublicKey();
            if (pubkey_33.size() != 33) {
                WLOG_ERR("[BIP84] Derived pubkey is not 33 bytes");
                return "";
            }

            // Convert std::array to std::vector for ComputeKeyID
            std::vector<uint8_t> pubkey_vec(pubkey_33.begin(), pubkey_33.end());
            key_id = wallet::ComputeKeyID(pubkey_vec);
            WLOG_INFO("[BIP84] Computed KeyID: " + wallet::KeyIDToHex(key_id.value()));

            // Convert to bytes for watch_scripts table
            for (size_t i = 0; i < script_pubkey.length(); i += 2) {
                std::string byte_str = script_pubkey.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
                script_bytes.push_back(byte);
            }
        }

        // Store address in database
        sqlite3_stmt* stmt = nullptr;
        const bool addresses_has_wallet_id = columnExists(db_, "addresses", "wallet_id");
        // Week 1 Day 2: Added KeyID columns for descriptor wallet foundation
        // Week 1 Day 3: Added script_pubkey column for Bitcoin-grade ownership (not address strings)
        const char* sql = addresses_has_wallet_id
            ? "INSERT INTO addresses (wallet_id, account, change, idx, address, pubkey, label, type, script_pubkey, key_id, internal_key_id, output_key_id, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
            : "INSERT INTO addresses (account, change, idx, address, pubkey, label, type, script_pubkey, key_id, internal_key_id, output_key_id, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

        // Determine address type string for database
        std::string addr_type_db = (effective_address_type == "taproot") ? "p2tr" : "p2wpkh";

        WLOG_INFO("💾 Attempting to INSERT address: type=" + addr_type_db + ", addr=" + address + ", idx=" + std::to_string(next_index));
        WLOG_INFO("💾 Database pointer: " + std::string(db_ ? "VALID" : "NULL") + ", wallet_id=" + std::to_string(current_wallet_id_));

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            int bind_index = 1;
            if (addresses_has_wallet_id) {
                sqlite3_bind_int(stmt, bind_index++, 1); // per-wallet DB always uses wallet_id=1
            }
            sqlite3_bind_int(stmt, bind_index++, 0); // account 0
            sqlite3_bind_int(stmt, bind_index++, 0); // external chain
            sqlite3_bind_int(stmt, bind_index++, next_index);
            sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_null(stmt, bind_index++); // pubkey (not used yet, reserved for future)
            sqlite3_bind_text(stmt, bind_index++, label.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind_index++, addr_type_db.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind_index++, script_pubkey.c_str(), -1, SQLITE_TRANSIENT);  // Bitcoin-grade: scriptPubKey for ownership

            // Week 1 Day 2: Bind KeyID columns (20 bytes each, or NULL)
            if (key_id.has_value()) {
                sqlite3_bind_blob(stmt, bind_index++, key_id->data(), key_id->size(), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, bind_index++);
            }

            if (internal_key_id.has_value()) {
                sqlite3_bind_blob(stmt, bind_index++, internal_key_id->data(), internal_key_id->size(), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, bind_index++);
            }

            if (output_key_id.has_value()) {
                sqlite3_bind_blob(stmt, bind_index++, output_key_id->data(), output_key_id->size(), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, bind_index++);
            }

            sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));

            int step_result = sqlite3_step(stmt);
            if (step_result != SQLITE_DONE) {
                std::string error_msg = "Failed to store address in database: ";
                error_msg += sqlite3_errmsg(db_);
                error_msg += " (sqlite3_step returned " + std::to_string(step_result) + ")";
                sqlite3_finalize(stmt);
                WLOG_ERR("💾 ❌ " + error_msg);
                return "";
            }
            WLOG_INFO("💾 ✅ Successfully stored address in database");
            sqlite3_finalize(stmt);
        } else {
            WLOG_ERR("💾 ❌ Failed to prepare SQL for address storage: " + std::string(sqlite3_errmsg(db_)));
            return "";
        }

        // Store derivation path in address_derivation_paths table
        uint32_t purpose = (effective_address_type == "taproot") ? 86 : 84;
        std::string derivation_path = "m/" + std::to_string(purpose) + "'/" +
                                      std::to_string(dinero::consensus::DINERO_COIN_TYPE) +
                                      "'/0'/0/" + std::to_string(next_index);
        const bool derivation_has_wallet_id = columnExists(db_, "address_derivation_paths", "wallet_id");
        const char* path_sql = derivation_has_wallet_id
            ? "INSERT INTO address_derivation_paths (address, wallet_id, derivation_path, script_pubkey, account, change, address_index, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
            : "INSERT INTO address_derivation_paths (address, derivation_path, script_pubkey, account, change, address_index, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)";

        if (sqlite3_prepare_v2(db_, path_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            int bind_index = 1;
            sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_TRANSIENT);
            if (derivation_has_wallet_id) {
                sqlite3_bind_int(stmt, bind_index++, 1); // per-wallet DB always uses wallet_id=1
            }
            sqlite3_bind_text(stmt, bind_index++, derivation_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind_index++, script_pubkey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, bind_index++, 0); // account
            sqlite3_bind_int(stmt, bind_index++, 0); // external chain
            sqlite3_bind_int(stmt, bind_index++, next_index);
            sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));

            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Add to watch_scripts table
        const char* watch_sql = "INSERT OR IGNORE INTO watch_scripts (script_pubkey, path, is_change, last_seen_height, created_at) VALUES (?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, watch_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_blob(stmt, 1, script_bytes.data(), script_bytes.size(), SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, derivation_path.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, 0); // not change
            sqlite3_bind_int(stmt, 4, 0);
            sqlite3_bind_int64(stmt, 5, std::time(nullptr));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Register with UTXOIndex if available (daemon mode).
        // In standalone/test contexts, WalletManager can operate without UTXOIndex.
        if (utxo_index_) {
            utxo_index_->RegisterAddress(script_bytes, derivation_path);
            WLOG_INFO("[wallet.getnewaddress] ✅ Registered address " + address + " with UTXOIndex");
        } else {
            WLOG_WARN("[wallet.getnewaddress] UTXOIndex not initialized; skipping address registration (standalone mode)");
        }

        WLOG_INFO("✅ Generated HD address: " + address + " at path: " + derivation_path);
        return address;

    } catch (const std::exception& e) {
        WLOG_ERR("Failed to generate HD address: " + std::string(e.what()));
        return "";
    }
}

std::string WalletManager::getNewChangeAddress(const std::string& label, const std::string& address_type) {
    if (!hasActiveWallet()) {
        WLOG_ERR("No active wallet for change address generation");
        return "";
    }

    if (current_wallet_id_ == -1) {
        WLOG_ERR("No wallet ID set for change address generation");
        return "";
    }

    std::string effective_address_type = "taproot";
    if (!address_type.empty()) {
        std::string requested;
        requested.reserve(address_type.size());
        for (char c : address_type) {
            requested.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (requested != "taproot" && requested != "p2tr" && requested != "bech32m" &&
            requested != "witness_v1_taproot") {
            WLOG_WARN("Taproot-only mode: ignoring non-taproot change address_type request '" + address_type + "'");
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // Generate change address using BIP84 (legacy) or BIP86 (taproot)
    // ═══════════════════════════════════════════════════════════════

    // Check if master seed is available
    if (master_seed_.empty()) {
        WLOG_ERR("HD wallet not initialized - master seed not available");
        return "";
    }

    try {
        // Get next change address index
        int next_index = getNextAddressIndex(0, 1);

        // Create master key from seed
        auto master_key = dinero::crypto::HDKeychain::fromSeed(master_seed_);

        std::string address;
        std::string script_pubkey;
        std::vector<uint8_t> script_bytes;

        // Week 1 Day 2: Compute KeyIDs for descriptor wallet
        std::optional<wallet::KeyID> key_id;
        std::optional<wallet::KeyID> internal_key_id;
        std::optional<wallet::KeyID> output_key_id;

        if (effective_address_type == "taproot") {
            // BIP86 change: m/86'/1448'/0'/1/index
            auto account_key = master_key.derive(86 | 0x80000000);
            auto coin_key = account_key.derive(dinero::consensus::DINERO_COIN_TYPE | 0x80000000);
            auto account0_key = coin_key.derive(0 | 0x80000000);
            auto change_key = account0_key.derive(1);  // change chain
            auto derived_key = change_key.derive(static_cast<uint32_t>(next_index));

            auto pubkey = derived_key.getPublicKey();
            if (pubkey.size() != 33) {
                WLOG_ERR("[Taproot] Derived change pubkey is not 33 bytes");
                return "";
            }

            std::vector<uint8_t> xonly_pubkey(pubkey.begin() + 1, pubkey.end());
            std::array<uint8_t, 32> output_key{};
            if (!ComputeTaprootOutputKey(xonly_pubkey, output_key, logger_)) {
                return "";
            }

            // Week 1 Day 2: Compute KeyIDs for Taproot change address
            std::array<uint8_t, 32> xonly_internal;
            std::copy(xonly_pubkey.begin(), xonly_pubkey.end(), xonly_internal.begin());
            internal_key_id = wallet::ComputeKeyIDFromXOnly(xonly_internal);
            output_key_id = wallet::ComputeKeyIDFromXOnly(output_key);
            key_id = internal_key_id.value();

            std::string hrp = dinero::HrpForActiveNetworkRef();
            if (hrp.empty()) {
                hrp = "din";
            }

            std::vector<uint8_t> witness_program(output_key.begin(), output_key.end());
            address = bech32::Encode(hrp, 1, witness_program, bech32::Encoding::BECH32M);
            if (address.empty()) {
                WLOG_ERR("[Taproot] Failed to encode Taproot change address");
                return "";
            }

            script_bytes.push_back(0x51);
            script_bytes.push_back(0x20);
            script_bytes.insert(script_bytes.end(), output_key.begin(), output_key.end());
            script_pubkey = "5120" + bytesToHex(output_key.data(), output_key.size());
        } else {
            // BIP84 change: m/84'/1448'/0'/1/index
            auto derived_key = dinero::crypto::HDKeychain::deriveBIP84(
                master_key,
                dinero::consensus::DINERO_COIN_TYPE,
                0,  // account 0
                1,  // internal chain (change addresses)
                static_cast<uint32_t>(next_index)
            );

            // Get address and public key
            address = derived_key.getAddress(dinero::HrpForActiveNetworkRef());
            auto hash160 = derived_key.getHash160();

            // Create scriptPubKey (P2WPKH: 0014 + 20-byte hash)
            script_pubkey = "0014" + bytesToHex(hash160.data(), hash160.size());

            // Week 1 Day 2: Compute KeyID for P2WPKH change address
            auto pubkey_33 = derived_key.getPublicKey();
            if (pubkey_33.size() == 33) {
                std::vector<uint8_t> pubkey_vec(pubkey_33.begin(), pubkey_33.end());
                key_id = wallet::ComputeKeyID(pubkey_vec);
            }

            // Convert to bytes for watch_scripts table
            for (size_t i = 0; i < script_pubkey.length(); i += 2) {
                std::string byte_str = script_pubkey.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
                script_bytes.push_back(byte);
            }
        }

        // Store address in database
        sqlite3_stmt* stmt = nullptr;
        const bool addresses_has_wallet_id = columnExists(db_, "addresses", "wallet_id");
        // Week 1 Day 3: Added script_pubkey and KeyID columns
        const char* sql = addresses_has_wallet_id
            ? "INSERT INTO addresses (wallet_id, account, change, idx, address, pubkey, label, type, script_pubkey, key_id, internal_key_id, output_key_id, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
            : "INSERT INTO addresses (account, change, idx, address, pubkey, label, type, script_pubkey, key_id, internal_key_id, output_key_id, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

        std::string addr_type_db = (effective_address_type == "taproot") ? "p2tr" : "p2wpkh";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            int bind_index = 1;
            if (addresses_has_wallet_id) {
                sqlite3_bind_int(stmt, bind_index++, 1); // per-wallet DB always uses wallet_id=1
            }
            sqlite3_bind_int(stmt, bind_index++, 0); // account 0
            sqlite3_bind_int(stmt, bind_index++, 1); // internal chain (change)
            sqlite3_bind_int(stmt, bind_index++, next_index);
            sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_null(stmt, bind_index++); // pubkey (reserved for future)
            sqlite3_bind_text(stmt, bind_index++, label.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind_index++, addr_type_db.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind_index++, script_pubkey.c_str(), -1, SQLITE_TRANSIENT);

            // Bind KeyID columns
            if (key_id.has_value()) {
                sqlite3_bind_blob(stmt, bind_index++, key_id->data(), key_id->size(), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, bind_index++);
            }

            if (internal_key_id.has_value()) {
                sqlite3_bind_blob(stmt, bind_index++, internal_key_id->data(), internal_key_id->size(), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, bind_index++);
            }

            if (output_key_id.has_value()) {
                sqlite3_bind_blob(stmt, bind_index++, output_key_id->data(), output_key_id->size(), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, bind_index++);
            }

            sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                WLOG_ERR("Failed to store change address in database");
                return "";
            }
            sqlite3_finalize(stmt);
        } else {
            WLOG_ERR("Failed to prepare SQL for change address storage");
            return "";
        }

        // Store derivation path in address_derivation_paths table
        uint32_t purpose = (effective_address_type == "taproot") ? 86 : 84;
        std::string derivation_path = "m/" + std::to_string(purpose) + "'/" +
                                      std::to_string(dinero::consensus::DINERO_COIN_TYPE) +
                                      "'/0'/1/" + std::to_string(next_index);
        const bool derivation_has_wallet_id = columnExists(db_, "address_derivation_paths", "wallet_id");
        const char* path_sql = derivation_has_wallet_id
            ? "INSERT INTO address_derivation_paths (address, wallet_id, derivation_path, script_pubkey, account, change, address_index, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
            : "INSERT INTO address_derivation_paths (address, derivation_path, script_pubkey, account, change, address_index, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)";

        if (sqlite3_prepare_v2(db_, path_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            int bind_index = 1;
            sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_TRANSIENT);
            if (derivation_has_wallet_id) {
                sqlite3_bind_int(stmt, bind_index++, 1); // per-wallet DB always uses wallet_id=1
            }
            sqlite3_bind_text(stmt, bind_index++, derivation_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind_index++, script_pubkey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, bind_index++, 0); // account
            sqlite3_bind_int(stmt, bind_index++, 1); // internal chain (change)
            sqlite3_bind_int(stmt, bind_index++, next_index);
            sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                WLOG_WARN("Failed to store change derivation path: " + std::string(sqlite3_errmsg(db_)));
            }
            sqlite3_finalize(stmt);
        } else {
            WLOG_WARN("Failed to prepare SQL for change derivation path storage");
        }

        // Add to watch_scripts table
        const char* watch_sql = "INSERT OR IGNORE INTO watch_scripts (script_pubkey, path, is_change, last_seen_height, created_at) VALUES (?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, watch_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_blob(stmt, 1, script_bytes.data(), script_bytes.size(), SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, derivation_path.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, 1); // is change
            sqlite3_bind_int(stmt, 4, 0);
            sqlite3_bind_int64(stmt, 5, std::time(nullptr));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Register with UTXOIndex if available (daemon mode).
        if (utxo_index_) {
            utxo_index_->RegisterAddress(script_bytes, derivation_path);
            WLOG_INFO("[wallet.getnewchangeaddress] ✅ Registered change address " + address + " with UTXOIndex");
        } else {
            WLOG_WARN("[wallet.getnewchangeaddress] UTXOIndex not initialized; skipping change address registration (standalone mode)");
        }

        WLOG_INFO("✅ Generated HD change address: " + address + " at path: " + derivation_path);
        return address;

    } catch (const std::exception& e) {
        WLOG_ERR("Failed to generate HD change address: " + std::string(e.what()));
        return "";
    }
}

// UTXO Management for spending (using WalletManager::WalletUTXO from header)

std::vector<WalletManager::WalletUTXO> WalletManager::getAvailableUTXOs() const {
    std::vector<WalletManager::WalletUTXO> utxos;
    
    if (current_wallet_id_ == -1) {
        return utxos;
    }
    
    // Get current blockchain height for maturity calculation
    uint32_t current_height = current_blockchain_height_;
    
    // Attach blockchain database and query UTXOs via join with watch_scripts
    const char* attach_sql = "ATTACH DATABASE './blockchain.db' AS chain";
    if (sqlite3_exec(db_, attach_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        WLOG_ERR("getAvailableUTXOs: Failed to attach blockchain database: " + std::string(sqlite3_errmsg(db_)));
        return utxos;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = R"(
        SELECT 
            lower(hex(u.tx_hash)) AS txid,
            u.output_index AS vout,
            u.amount,
            u.block_height AS height,
            u.is_coinbase,
            w.path,
            w.is_change,
            lower(hex(u.script_pubkey)) AS script_pubkey_hex
        FROM chain.utxo u
        JOIN watch_scripts w ON u.script_pubkey = w.script_pubkey
        WHERE (u.is_coinbase = 0) OR (u.block_height <= ?1 - 100)
        ORDER BY u.amount ASC
    )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getAvailableUTXOs: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        sqlite3_exec(db_, "DETACH DATABASE chain", nullptr, nullptr, nullptr);
        return utxos;
    }

    // Bind current height for coinbase maturity calculation
    rc = sqlite3_bind_int(stmt, 1, current_height);
    if (rc != SQLITE_OK) {
        WLOG_ERR("getAvailableUTXOs: bind failed: " + std::string(sqlite3_errmsg(db_)));
        sqlite3_finalize(stmt);
        sqlite3_exec(db_, "DETACH DATABASE chain", nullptr, nullptr, nullptr);
        return utxos;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        // SEATBELT: Validate txid before use
        const char* txid_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!txid_cstr || strlen(txid_cstr) == 0) {
            logCorruptRow("chain.utxo", "tx_hash", "NULL or empty txid");
            continue;  // Skip this row, continue with others
        }

        WalletManager::WalletUTXO utxo;
        utxo.txid = txid_cstr;
        utxo.vout = sqlite3_column_int(stmt, 1);
        utxo.amount_una = sqlite3_column_int64(stmt, 2);
        utxo.amount_din = static_cast<double>(utxo.amount_una) / dinero::ConsensusSubsidy::UNA_PER_DIN;
        utxo.height = sqlite3_column_int(stmt, 3);
        utxo.is_coinbase = sqlite3_column_int(stmt, 4) != 0;
        utxo.confirmations = current_height - utxo.height + 1;

        // Get address from path (simplified - in real implementation derive from path)
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        utxo.address = path ? std::string(path) : "unknown";
        utxo.derivation_path = path ? std::string(path) : "";

        const char* script_pubkey_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        utxo.script_pubkey = script_pubkey_hex ? std::string(script_pubkey_hex) : "";
        
        // Calculate maturity and spendability
        utxo.is_mature = !utxo.is_coinbase || (utxo.confirmations >= 100);
        utxo.spendable = utxo.confirmations >= 1 && utxo.is_mature;
        utxo.label = "";
        utxo.is_spent = false;

        if (utxo.spendable) {
            utxos.push_back(utxo);
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "DETACH DATABASE chain", nullptr, nullptr, nullptr);
    
    WLOG_INFO("getAvailableUTXOs: Found " + std::to_string(utxos.size()) + " spendable UTXOs from chainstate");
    return utxos;
}

// ============================================================================
// Phase W.2.6: Wallet Scan Status API
// ============================================================================

/**
 * @brief Get current wallet scan status
 *
 * Returns snapshot of wallet scan progress for sync UX.
 * Thread-safe, read-only.
 */
WalletManager::WalletScanStatus WalletManager::GetScanStatus(uint32_t chain_height) const {
    WalletScanStatus status;

    // Use provided chain_height or fall back to cached value
    status.chain_height = (chain_height > 0) ? chain_height : current_blockchain_height_;

    if (!db_) {
        status.scan_height = 0;
        status.is_scanning = false;
        return status;
    }

    // Query sync_meta table for actual rescan progress
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT last_scanned_height, scan_complete FROM sync_meta WHERE id = 1";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            status.scan_height = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
            int scan_complete = sqlite3_column_int(stmt, 1);
            // is_scanning = true if scan started but not complete, and not at tip
            status.is_scanning = (scan_complete == 0) && (status.scan_height < status.chain_height);
        } else {
            // No sync_meta entry - never scanned
            status.scan_height = 0;
            status.is_scanning = false;
        }
        sqlite3_finalize(stmt);
    } else {
        status.scan_height = 0;
        status.is_scanning = false;
    }

    return status;
}

// ============================================================================
// Wallet rescan functionality
// ============================================================================
// Architecture: Wallet is a READ-ONLY consumer of validated blocks.
// Direction: Consensus UTXO (authoritative) -> Wallet index (derived, rebuildable)
// This function scans the canonical chain and populates wallet-local state.
// ============================================================================

bool WalletManager::rescanBlockchain(int start_height,
                                     int gap_limit,
                                     dinero::ChainDB* chain_db,
                                     dinero::BlockStorage* block_storage) {
    if (!chain_db) {
        WLOG_ERR("rescanBlockchain: ChainDB is null - cannot scan");
        return false;
    }

    if (!db_) {
        WLOG_ERR("rescanBlockchain: Wallet database not initialized");
        return false;
    }

    // Get chain tip
    auto tip_result = chain_db->getTip();
    if (tip_result.status() != dinero::Status::Ok) {
        WLOG_ERR("rescanBlockchain: Failed to get chain tip");
        return false;
    }
    uint32_t tip_height = tip_result.value().height;

    if (start_height < 0) start_height = 0;
    if (static_cast<uint32_t>(start_height) > tip_height) {
        WLOG_INFO("rescanBlockchain: start_height (" + std::to_string(start_height) +
                  ") > tip (" + std::to_string(tip_height) + "), nothing to scan");
        return true;
    }

    WLOG_INFO("═══════════════════════════════════════════════════════════");
    WLOG_INFO("  WALLET RESCAN: height " + std::to_string(start_height) +
              " to " + std::to_string(tip_height));
    WLOG_INFO("═══════════════════════════════════════════════════════════");

    // Load watch_scripts into memory for fast matching
    std::set<std::vector<uint8_t>> watch_scripts;
    {
        const char* sql = "SELECT script_pubkey FROM watch_scripts";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const void* blob = sqlite3_column_blob(stmt, 0);
                int blob_size = sqlite3_column_bytes(stmt, 0);
                if (blob && blob_size > 0) {
                    std::vector<uint8_t> script(
                        static_cast<const uint8_t*>(blob),
                        static_cast<const uint8_t*>(blob) + blob_size
                    );
                    watch_scripts.insert(script);
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // GAP LIMIT: Derive addresses if watch_scripts is under-populated.
    // After wallet restore, only index-0 may exist. We must derive up to
    // gap_limit for both external (receive) and internal (change) chains
    // so the rescan can discover all historical UTXOs.
    // ═══════════════════════════════════════════════════════════════════════
    const int expected_scripts = gap_limit * 2;  // external + change
    if (static_cast<int>(watch_scripts.size()) < expected_scripts && !master_seed_.empty()) {
        WLOG_INFO("rescanBlockchain: watch_scripts (" + std::to_string(watch_scripts.size()) +
                  ") < gap_limit*2 (" + std::to_string(expected_scripts) +
                  "), deriving addresses for discovery");

        int current_external = getNextAddressIndex(0, 0);  // how many external already exist
        int current_change = getNextAddressIndex(0, 1);     // how many change already exist

        // Derive external addresses up to gap_limit
        for (int i = current_external; i < gap_limit; ++i) {
            std::string addr = getNewAddress("", "taproot");
            if (addr.empty()) break;
        }
        // Derive change addresses up to gap_limit
        for (int i = current_change; i < gap_limit; ++i) {
            std::string addr = getNewChangeAddress("", "taproot");
            if (addr.empty()) break;
        }

        // Reload watch_scripts after derivation
        watch_scripts.clear();
        {
            const char* sql2 = "SELECT script_pubkey FROM watch_scripts";
            sqlite3_stmt* stmt2 = nullptr;
            if (sqlite3_prepare_v2(db_, sql2, -1, &stmt2, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt2) == SQLITE_ROW) {
                    const void* blob = sqlite3_column_blob(stmt2, 0);
                    int blob_size = sqlite3_column_bytes(stmt2, 0);
                    if (blob && blob_size > 0) {
                        std::vector<uint8_t> script(
                            static_cast<const uint8_t*>(blob),
                            static_cast<const uint8_t*>(blob) + blob_size
                        );
                        watch_scripts.insert(script);
                    }
                }
                sqlite3_finalize(stmt2);
            }
        }
        WLOG_INFO("rescanBlockchain: after derivation, loaded " + std::to_string(watch_scripts.size()) + " watch scripts");
    }

    if (watch_scripts.empty()) {
        WLOG_WARN("rescanBlockchain: No watch_scripts registered - nothing to find");
        return true;
    }

    WLOG_INFO("Loaded " + std::to_string(watch_scripts.size()) + " watch scripts");

    // ═══════════════════════════════════════════════════════════════════════
    // REORG SAFETY: Wipe UTXOs >= start_height before scanning
    // ═══════════════════════════════════════════════════════════════════════
    // The chain before start_height is canonical NOW, but might have been
    // different during a previous scan. We must invalidate any wallet state
    // that could be stale from a reorged chain.
    // ═══════════════════════════════════════════════════════════════════════
    {
        WLOG_INFO("Reorg safety: clearing UTXOs >= height " + std::to_string(start_height));

        // Delete UTXOs found at or after start_height
        const char* delete_utxos_sql = "DELETE FROM utxos WHERE height >= ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, delete_utxos_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, start_height);
            sqlite3_step(stmt);
            int deleted = sqlite3_changes(db_);
            sqlite3_finalize(stmt);
            if (deleted > 0) {
                WLOG_INFO("  Cleared " + std::to_string(deleted) + " UTXOs from reorg-unsafe heights");
            }
        }

        // Also un-mark UTXOs that were "spent" at heights >= start_height
        // (the spend might have been on a reorged chain)
        const char* unspend_sql = "UPDATE utxos SET is_spent = 0, spent_txid = NULL, spent_height = NULL WHERE spent_height >= ?";
        if (sqlite3_prepare_v2(db_, unspend_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, start_height);
            sqlite3_step(stmt);
            int unspent = sqlite3_changes(db_);
            sqlite3_finalize(stmt);
            if (unspent > 0) {
                WLOG_INFO("  Unmarked " + std::to_string(unspent) + " UTXOs as unspent (reorg recovery)");
            }
        }
    }

    // wallet_id column guaranteed by v24 migration

    // Track progress
    uint32_t blocks_scanned = 0;
    uint32_t utxos_found = 0;
    uint32_t utxos_spent = 0;

    // Begin transaction for batch inserts
    exec(db_, "BEGIN TRANSACTION");

    // Scan blocks deterministically (no parallelism - correctness > speed)
    for (uint32_t height = static_cast<uint32_t>(start_height); height <= tip_height; ++height) {
        // Get block hash at height
        auto hash_result = chain_db->getBlockHashByHeight(height);
        if (hash_result.status() != dinero::Status::Ok) {
            WLOG_ERR("rescanBlockchain: Failed to get hash at height " + std::to_string(height));
            exec(db_, "ROLLBACK");
            return false;
        }

        // Get full block
        auto block_result = dinero::storage::ReadArchivalBlock(*chain_db, block_storage, hash_result.value());
        if (block_result.status() != dinero::Status::Ok) {
            WLOG_ERR("rescanBlockchain: Failed to get block at height " + std::to_string(height));
            exec(db_, "ROLLBACK");
            return false;
        }

        const dinero::Block& block = block_result.value();

        // Process each transaction in the block
        for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
            const dinero::Transaction& tx = block.vtx[tx_idx];
            std::string txid_hex = tx.GetTxid().AsUint256().GetHex();
            bool is_coinbase = (tx_idx == 0);

            // Check outputs - do we own any?
            for (size_t vout_idx = 0; vout_idx < tx.vout.size(); ++vout_idx) {
                const dinero::TxOutput& output = tx.vout[vout_idx];

                if (watch_scripts.count(output.scriptPubKey) > 0) {
                    // This output is ours! Add to utxos table
                    std::string script_pubkey_hex;
                    script_pubkey_hex.reserve(output.scriptPubKey.size() * 2);
                    static constexpr char kHex[] = "0123456789abcdef";
                    for (uint8_t b : output.scriptPubKey) {
                        script_pubkey_hex.push_back(kHex[(b >> 4) & 0x0F]);
                        script_pubkey_hex.push_back(kHex[b & 0x0F]);
                    }

                    std::string address = extractAddressFromScript(output.scriptPubKey);
                    if (address.empty()) {
                        // Preserve row integrity even for unknown script templates.
                        address = "script:" + script_pubkey_hex.substr(0, 16);
                    }

                    const char* insert_sql = R"(
                        INSERT OR IGNORE INTO utxos
                        (wallet_id, txid, vout, address, amount, script_pubkey, height, is_coinbase, is_spent, created_at)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?)
                    )";

                    sqlite3_stmt* stmt = nullptr;
                    if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        int bind_index = 1;
                        sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
                        sqlite3_bind_text(stmt, bind_index++, txid_hex.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, bind_index++, static_cast<int>(vout_idx));
                        sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int64(stmt, bind_index++, static_cast<int64_t>(output.value.GetUna()));
                        sqlite3_bind_text(stmt, bind_index++, script_pubkey_hex.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, bind_index++, static_cast<int>(height));
                        sqlite3_bind_int(stmt, bind_index++, is_coinbase ? 1 : 0);
                        sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));

                        if (sqlite3_step(stmt) == SQLITE_DONE) {
                            utxos_found++;
                        }
                        sqlite3_finalize(stmt);
                    }
                }
            }

            // Check inputs - are any of our UTXOs being spent?
            if (!is_coinbase) {
                for (const dinero::TxInput& input : tx.vin) {
                    std::string prev_txid = input.prevout.txid.AsUint256().GetHex();
                    uint32_t prev_vout = input.prevout.vout;

                    // Mark as spent if we own this UTXO
                    const char* spend_sql = R"(
                        UPDATE utxos SET is_spent = 1, spent_txid = ?, spent_height = ?
                        WHERE wallet_id = ? AND txid = ? AND vout = ? AND is_spent = 0
                    )";

                    sqlite3_stmt* stmt = nullptr;
                    if (sqlite3_prepare_v2(db_, spend_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        int bind_index = 1;
                        sqlite3_bind_text(stmt, bind_index++, txid_hex.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, bind_index++, static_cast<int>(height));
                        sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
                        sqlite3_bind_text(stmt, bind_index++, prev_txid.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(stmt, bind_index++, static_cast<int>(prev_vout));

                        if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0) {
                            utxos_spent++;
                        }
                        sqlite3_finalize(stmt);
                    }
                }
            }
        }

        std::string shielded_error;
        if (!wallet::shielded_ops::RescanConfirmedBlock(*this, height,
                                                        block.vtx,
                                                        &shielded_error)) {
            exec(db_, "ROLLBACK");
            WLOG_ERR("rescanBlockchain: Shielded rescan failed at height " +
                     std::to_string(height) + ": " + shielded_error);
            return false;
        }

        blocks_scanned++;

        // Progress logging every 100 blocks
        if (blocks_scanned % 100 == 0) {
            WLOG_INFO("  Scanned " + std::to_string(blocks_scanned) + " blocks, " +
                      "found " + std::to_string(utxos_found) + " UTXOs, " +
                      "spent " + std::to_string(utxos_spent));
        }

        // Update sync_meta progress (resumable)
        if (blocks_scanned % 100 == 0 || height == tip_height) {
            const char* meta_sql = R"(
                INSERT OR REPLACE INTO sync_meta (id, last_scanned_height, birth_height, scan_complete)
                VALUES (1, ?, ?, ?)
            )";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, meta_sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, static_cast<int>(height));
                sqlite3_bind_int(stmt, 2, start_height);
                sqlite3_bind_int(stmt, 3, (height == tip_height) ? 1 : 0);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }

    // Commit all changes
    exec(db_, "COMMIT");

    // Update wallet tip
    current_blockchain_height_ = tip_height;

    WLOG_INFO("═══════════════════════════════════════════════════════════");
    WLOG_INFO("  RESCAN COMPLETE");
    WLOG_INFO("  Blocks scanned: " + std::to_string(blocks_scanned));
    WLOG_INFO("  UTXOs found: " + std::to_string(utxos_found));
    WLOG_INFO("  UTXOs spent: " + std::to_string(utxos_spent));
    WLOG_INFO("═══════════════════════════════════════════════════════════");

    return true;
}

// ============================================================================
// UTXO-set rescan (AssumeUTXO / snapshot bootstrap)
// ============================================================================
// Complements rescanBlockchain(): the block-replay rescan matches outputs from
// block transaction bodies, but a snapshot-bootstrapped node has no pre-snapshot
// block bodies, so coins received before the snapshot height are invisible to
// the wallet. This scans the loaded chainstate UTXO set directly and records any
// coins owned by this wallet, reusing the exact insert logic of rescanBlockchain.
// ============================================================================
int WalletManager::rescanUtxoSet(
    const std::function<void(const std::function<void(const UtxoSetEntry&)>&)>& produce,
    uint32_t snapshot_height) {
    if (!db_) {
        WLOG_ERR("rescanUtxoSet: wallet database not initialized");
        return 0;
    }
    if (current_wallet_id_ == -1) {
        WLOG_WARN("rescanUtxoSet: no active wallet (current_wallet_id_ == -1)");
        return 0;
    }

    // Load watch_scripts into memory for fast matching (same source as the
    // block-replay rescan at rescanBlockchain()).
    std::set<std::vector<uint8_t>> watch_scripts;
    {
        const char* sql = "SELECT script_pubkey FROM watch_scripts";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const void* blob = sqlite3_column_blob(stmt, 0);
                int blob_size = sqlite3_column_bytes(stmt, 0);
                if (blob && blob_size > 0) {
                    watch_scripts.emplace(
                        static_cast<const uint8_t*>(blob),
                        static_cast<const uint8_t*>(blob) + blob_size);
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    if (watch_scripts.empty()) {
        WLOG_WARN("rescanUtxoSet: no watch_scripts registered - nothing to match");
        // Still advance the watermark below so catch-up starts above the snapshot.
    }

    WLOG_INFO("rescanUtxoSet: scanning snapshot UTXO set against " +
              std::to_string(watch_scripts.size()) + " watch script(s)");

    // Ensure the snapshot_anchored column exists (idempotent; older wallet DBs
    // predate it). Coins flagged anchored are trusted by the balance read path
    // so its "not in live utxo_index_ => spent" inference never re-clobbers them
    // (snapshot UTXOs live in the utreexo accumulator, never the live index).
    sqlite3_exec(db_, "ALTER TABLE utxos ADD COLUMN snapshot_anchored INTEGER NOT NULL DEFAULT 0",
                 nullptr, nullptr, nullptr);

    int recorded = 0;
    exec(db_, "BEGIN TRANSACTION");

    // Per-coin sink: identical insert path to rescanBlockchain (hex-encode the
    // scriptPubKey, extractAddressFromScript, upsert INTO utxos).
    auto sink = [&](const UtxoSetEntry& e) {
        if (watch_scripts.find(e.script_pubkey) == watch_scripts.end()) {
            return;  // not ours
        }

        std::string script_pubkey_hex;
        script_pubkey_hex.reserve(e.script_pubkey.size() * 2);
        static constexpr char kHex[] = "0123456789abcdef";
        for (uint8_t b : e.script_pubkey) {
            script_pubkey_hex.push_back(kHex[(b >> 4) & 0x0F]);
            script_pubkey_hex.push_back(kHex[b & 0x0F]);
        }

        std::string address = extractAddressFromScript(e.script_pubkey);
        if (address.empty()) {
            // Preserve row integrity even for unknown script templates.
            address = "script:" + script_pubkey_hex.substr(0, 16);
        }

        // A coin present in the snapshot UTXO set is UNSPENT by definition. If a
        // row already exists (e.g. the wallet's block-replay history, or a prior
        // failed block-rescan that wrongly flagged it is_spent=1), an INSERT OR
        // IGNORE would leave the stale is_spent intact and the balance reads 0.
        // Upsert instead: un-spend the row and refresh authoritative fields.
        // Coinbase maturity is judged against the snapshot base height.
        int is_mature = 1;
        if (e.is_coinbase && snapshot_height < e.height + 100) {
            is_mature = 0;
        }

        const char* insert_sql = R"(
            INSERT INTO utxos
            (wallet_id, txid, vout, address, amount, script_pubkey, height, is_coinbase, is_spent, is_mature, snapshot_anchored, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?, 1, ?)
            ON CONFLICT DO UPDATE SET
                is_spent = 0,
                spent_txid = NULL,
                spent_height = NULL,
                wallet_id = excluded.wallet_id,
                amount = excluded.amount,
                script_pubkey = excluded.script_pubkey,
                height = excluded.height,
                is_coinbase = excluded.is_coinbase,
                is_mature = excluded.is_mature,
                snapshot_anchored = 1
        )";

        sqlite3_stmt* stmt = nullptr;
        const int prepare_rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
        if (prepare_rc != SQLITE_OK) {
            throw std::runtime_error(
                "snapshot UTXO upsert prepare failed: " + std::string(sqlite3_errmsg(db_)));
        }

        int bind_index = 1;
        sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
        sqlite3_bind_text(stmt, bind_index++, e.txid_hex.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, bind_index++, static_cast<int>(e.vout));
        sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, bind_index++, static_cast<int64_t>(e.amount_una));
        sqlite3_bind_text(stmt, bind_index++, script_pubkey_hex.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, bind_index++, static_cast<int>(e.height));
        sqlite3_bind_int(stmt, bind_index++, e.is_coinbase ? 1 : 0);
        sqlite3_bind_int(stmt, bind_index++, is_mature);
        sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));

        const int step_rc = sqlite3_step(stmt);
        if (step_rc != SQLITE_DONE) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            throw std::runtime_error("snapshot UTXO upsert failed: " + error);
        }
        if (sqlite3_changes(db_) > 0) {
            recorded++;
        }
        sqlite3_finalize(stmt);
    };

    try {
        produce(sink);
        exec(db_, "COMMIT");
    } catch (const std::exception& e) {
        try {
            exec(db_, "ROLLBACK");
        } catch (const std::exception&) {
            // Preserve the original SQL/producer error below.
        }
        WLOG_ERR(std::string("rescanUtxoSet: atomic snapshot import failed: ") + e.what());
        return -1;
    }

    WLOG_INFO("rescanUtxoSet: recorded " + std::to_string(recorded) +
              " owned UTXO(s) from snapshot UTXO set");

    // Advance the wallet's scanned-height watermark to the snapshot base so a
    // later block-replay catch-up starts ABOVE the snapshot height instead of at
    // 0. Replaying pre-snapshot heights (whose block bodies are absent on a
    // fast-synced node) would re-spend/clear the coins we just recorded. Persist
    // unconditionally to sync_meta (the catch-up keys off last_scanned_height,
    // not the tip), and also bump the tip if the wallet is behind.
    persistScanHeight(snapshot_height);
    if (snapshot_height > current_blockchain_height_) {
        setBlockchainHeight(snapshot_height);
    }

    return recorded;
}

void WalletManager::loadBlockchainHeight() {
    if (!db_) {
        WLOG_WARN("Cannot load blockchain height: database not initialized");
        return;
    }

    sqlite3_stmt* stmt;
    const char* sql = "SELECT height FROM tip LIMIT 1";

    WLOG_INFO("Loading blockchain height from tip table...");

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            current_blockchain_height_ = sqlite3_column_int(stmt, 0);
            WLOG_INFO("Loaded blockchain height from database: " + std::to_string(current_blockchain_height_));
        } else {
            WLOG_WARN("No height found in tip table, using default 0");
            current_blockchain_height_ = 0;
        }
        sqlite3_finalize(stmt);
    } else {
        WLOG_ERR("Failed to prepare tip table query: " + std::string(sqlite3_errmsg(db_)));
        current_blockchain_height_ = 0;
    }
}

void WalletManager::persistTipHeight(uint32_t height) {
    if (!db_) return;
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO tip (rowid, height) VALUES (1, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, static_cast<int>(height));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void WalletManager::persistScanHeight(uint32_t height) {
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE sync_meta SET last_scanned_height = ? WHERE id = 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, static_cast<int>(height));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void WalletManager::runHealthCheck() {
    if (!db_) {
        WLOG_ERR("Health check failed: database not initialized");
        return;
    }

    // Run PRAGMA quick_check for fast integrity verification
    sqlite3_stmt* stmt;
    const char* sql = "PRAGMA quick_check";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (result && std::string(result) == "ok") {
                WLOG_INFO("Database health check passed: " + std::string(result));
            } else {
                WLOG_ERR("Database health check failed: " + std::string(result ? result : "unknown"));
            }
        } else {
            WLOG_ERR("Database health check failed: no result");
        }
        sqlite3_finalize(stmt);
    } else {
        WLOG_ERR("Database health check failed: " + std::string(sqlite3_errmsg(db_)));
    }
}

std::string WalletManager::runIntegrityCheck() {
    if (!db_) {
        return "Database not initialized";
    }

    // Run PRAGMA integrity_check for comprehensive verification
    sqlite3_stmt* stmt;
    const char* sql = "PRAGMA integrity_check";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string result;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* row = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (row) {
                if (!result.empty()) result += "\n";
                result += std::string(row);
            }
        }
        sqlite3_finalize(stmt);
        return result.empty() ? "No integrity check results" : result;
    } else {
        return "Integrity check failed: " + std::string(sqlite3_errmsg(db_));
    }
}

uint32_t WalletManager::getBlocksUntilMature(uint32_t utxo_height) const {
    if (utxo_height > current_blockchain_height_) {
        return 100; // UTXO from future block
    }
    
    uint32_t confirmations = current_blockchain_height_ - utxo_height + 1;
    if (confirmations >= 100) {
        return 0; // Already mature
    }
    
    return 100 - confirmations;
}

void WalletManager::checkFilePermissions() {
#ifdef _WIN32
    // File permissions (mode_t, chmod, umask) are POSIX-only.
    // Windows uses ACLs; skip permission enforcement on Windows.
    return;
#else
    if (!db_) {
        WLOG_WARN("Cannot check file permissions: database not initialized");
        return;
    }

    // Get database file path
    const char* db_path = sqlite3_db_filename(db_, "main");
    if (!db_path) {
        WLOG_WARN("Cannot get database file path for permission check");
        return;
    }

    std::filesystem::path db_file_path(db_path);
    std::filesystem::path db_dir = db_file_path.parent_path();

    bool adjusted = false;
    std::string perm_error;

    if (!EnsurePathPermissions(db_dir, 0700, &adjusted, &perm_error)) {
        throw std::runtime_error("Wallet directory permission enforcement failed: " + perm_error);
    }
    if (adjusted) {
        WLOG_WARN("Auto-corrected wallet directory permissions to 0700: " + db_dir.string());
    }

    adjusted = false;
    if (!EnsurePathPermissions(db_file_path, 0600, &adjusted, &perm_error)) {
        throw std::runtime_error("Wallet DB file permission enforcement failed: " + perm_error);
    }
    if (adjusted) {
        WLOG_WARN("Auto-corrected wallet database permissions to 0600: " + db_file_path.string());
    }

    for (const char* suffix : {"-wal", "-shm"}) {
        std::filesystem::path sidecar = db_file_path.string() + suffix;
        if (!std::filesystem::exists(sidecar)) {
            continue;
        }

        adjusted = false;
        if (!EnsurePathPermissions(sidecar, 0600, &adjusted, &perm_error)) {
            throw std::runtime_error("Wallet sidecar permission enforcement failed: " + perm_error);
        }
        if (adjusted) {
            WLOG_WARN("Auto-corrected wallet sidecar permissions to 0600: " + sidecar.string());
        }
    }
#endif // !_WIN32
}

void WalletManager::validateSchemaVersion() {
    if (!db_) {
        WLOG_ERR("Schema validation failed: database not initialized");
        return;
    }

    int db_version = getUserVersion(db_);
    int compiled_version = 7;  // SCHEMA_REV - Nov 2025: HD wallet + full BIP84 support

    if (db_version != compiled_version) {
        std::string error_msg = "Schema version mismatch: database=" + std::to_string(db_version) + 
                               ", compiled=" + std::to_string(compiled_version) + 
                               ". Use -allow-unsafe-db-mismatch to override (NOT RECOMMENDED)";
        
        WLOG_ERR(error_msg);
        
        // Check for override flag (would need to be passed from main)
        // For now, just log the error and continue
        WLOG_WARN("Continuing with schema mismatch - this may cause data corruption");
    } else {
        WLOG_INFO("Schema version validation passed: " + std::to_string(db_version));
    }
}

void WalletManager::updateUTXOMaturity() {
    if (!db_ || current_wallet_id_ == -1) {
        return;
    }
    
    // Update tip table with current blockchain height
    sqlite3_stmt* tip_stmt;
    const char* tip_sql = "INSERT OR REPLACE INTO tip (rowid, height) VALUES (1, ?)";
    int rc = sqlite3_prepare_v2(db_, tip_sql, -1, &tip_stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(tip_stmt, 1, current_blockchain_height_);
        sqlite3_step(tip_stmt);
        sqlite3_finalize(tip_stmt);
    }

    if (columnExists(db_, "transactions", "height") &&
        columnExists(db_, "transactions", "confirmations")) {
        const bool tx_has_wallet_id = columnExists(db_, "transactions", "wallet_id");
        sqlite3_stmt* tx_stmt = nullptr;
        const char* tx_sql_with_wallet = R"(
            UPDATE transactions
            SET confirmations = CASE
                WHEN height > 0 AND ? >= height THEN ? - height + 1
                ELSE confirmations
            END
            WHERE wallet_id = ? AND height > 0
        )";
        const char* tx_sql_no_wallet = R"(
            UPDATE transactions
            SET confirmations = CASE
                WHEN height > 0 AND ? >= height THEN ? - height + 1
                ELSE confirmations
            END
            WHERE height > 0
        )";

        if (sqlite3_prepare_v2(db_, tx_has_wallet_id ? tx_sql_with_wallet : tx_sql_no_wallet,
                               -1, &tx_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(tx_stmt, 1, current_blockchain_height_);
            sqlite3_bind_int(tx_stmt, 2, current_blockchain_height_);
            if (tx_has_wallet_id) {
                sqlite3_bind_int(tx_stmt, 3, current_wallet_id_);
            }
            sqlite3_step(tx_stmt);
            sqlite3_finalize(tx_stmt);
        }
    }
    
    // Per-wallet schema computes maturity dynamically in read-paths and may not
    // persist an `is_mature` column. Skip legacy UPDATE when column is absent.
    if (!columnExists(db_, "utxos", "is_mature")) {
        return;
    }

    // Legacy schema compatibility: some databases still include wallet_id.
    const bool has_wallet_id = columnExists(db_, "utxos", "wallet_id");

    // Update is_mature for coinbase UTXOs based on current blockchain height.
    sqlite3_stmt* stmt;
    const char* sql_with_wallet = R"(
        UPDATE utxos 
        SET is_mature = CASE 
            WHEN is_coinbase = 1 AND (? - height + 1) >= 100 THEN 1
            WHEN is_coinbase = 0 THEN 1
            ELSE 0
        END
        WHERE wallet_id = ? AND is_spent = 0
    )";
    const char* sql_no_wallet = R"(
        UPDATE utxos 
        SET is_mature = CASE 
            WHEN is_coinbase = 1 AND (? - height + 1) >= 100 THEN 1
            WHEN is_coinbase = 0 THEN 1
            ELSE 0
        END
        WHERE is_spent = 0
    )";
    
    rc = sqlite3_prepare_v2(db_, has_wallet_id ? sql_with_wallet : sql_no_wallet, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return;
    }
    
    sqlite3_bind_int(stmt, 1, current_blockchain_height_);
    if (has_wallet_id) {
        sqlite3_bind_int(stmt, 2, current_wallet_id_);
    }
    
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool WalletManager::addUTXO(const std::string& txid, int vout, int64_t amount,
                           const std::string& address, const std::string& script_pubkey,
                           int height, bool is_coinbase) {
    if (!db_ || current_wallet_id_ == -1) {
        WLOG_ERR("[addUTXO] ❌ No wallet selected (current_wallet_id_ == -1)");
        return false;
    }
    WLOG_INFO("[addUTXO] Attempting to add UTXO " + txid + ":" + std::to_string(vout) + " to wallet_id=" + std::to_string(current_wallet_id_));

    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT OR REPLACE INTO utxos
        (wallet_id, txid, vout, address, amount, script_pubkey, height, is_coinbase, is_spent, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?)
    )";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        WLOG_ERR("[addUTXO] ❌ SQL prepare failed: " + std::string(sqlite3_errmsg(db_)) + " (rc=" + std::to_string(rc) + ")");
        return false;
    }
    WLOG_INFO("[addUTXO] SQL prepared successfully, binding parameters...");

    int bind_index = 1;
    sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
    sqlite3_bind_text(stmt, bind_index++, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, bind_index++, vout);
    sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, bind_index++, amount);
    sqlite3_bind_text(stmt, bind_index++, script_pubkey.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, bind_index++, height);
    sqlite3_bind_int(stmt, bind_index++, is_coinbase ? 1 : 0);
    sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr));
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        WLOG_INFO("[addUTXO] ✅ Successfully added UTXO " + txid + ":" + std::to_string(vout));
        return true;
    } else {
        WLOG_ERR("[addUTXO] ❌ Failed to add UTXO " + txid + ":" + std::to_string(vout) +
                  " - SQLite error: " + std::string(sqlite3_errmsg(db_)) + " (rc=" + std::to_string(rc) + ")");
        return false;
    }
}

bool WalletManager::spendUTXO(const std::string& txid, int vout) {
    if (current_wallet_id_ == -1) {
        return false;
    }

    sqlite3_stmt* stmt;
    // Note: Per-wallet database - no wallet_id column needed
    const char* sql = "UPDATE utxos SET is_spent = 1 WHERE txid = ? AND vout = ?";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, vout);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

// Phase 4B: Remove UTXO from database (for reorg rollback)
bool WalletManager::removeUTXO(const std::string& txid, int vout) {
    if (current_wallet_id_ == -1) {
        return false;
    }

    sqlite3_stmt* stmt;
    // Note: Per-wallet database - no wallet_id column needed
    const char* sql = "DELETE FROM utxos WHERE txid = ? AND vout = ?";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, vout);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

// ═══════════════════════════════════════════════════════════════
// Phase 3: HD Wallet Private Key Derivation - WalletManager Integration
// ═══════════════════════════════════════════════════════════════

std::optional<std::vector<uint8_t>> WalletManager::deriveKeyForScriptPubKey(const std::string& script_pubkey) {
    // ⚠️ OWNERSHIP LOGIC - Uses scriptPubKey (consensus data), NOT address (display string)
    // Check if wallet is active and unlocked
    if (!hasActiveWallet()) {
        WLOG_ERR("No active wallet");
        return std::nullopt;
    }

    if (wallet_locked_) {
        WLOG_ERR("Wallet is locked - cannot access private keys");
        return std::nullopt;
    }

    // Check cache first for performance
    auto cache_it = private_key_cache_.find(script_pubkey);
    if (cache_it != private_key_cache_.end()) {
        WLOG_DEBUG("Private key found in cache for scriptPubKey: " + script_pubkey);
        return cache_it->second;
    }

    // Get derivation path from database (by scriptPubKey, NOT address)
    auto path_opt = getDerivationPath(script_pubkey);
    if (!path_opt) {
        // Fallback: watch_scripts stores authoritative script->path bindings used
        // by UTXO discovery, even when addresses/address_derivation_paths were
        // lost or compacted in older wallets.
        std::vector<unsigned char> script_bytes;
        if (util::unhex(script_pubkey, script_bytes) && !script_bytes.empty()) {
            sqlite3_stmt* watch_stmt = nullptr;
            const char* watch_sql = "SELECT path FROM watch_scripts WHERE script_pubkey = ? LIMIT 1";
            if (sqlite3_prepare_v2(db_, watch_sql, -1, &watch_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_blob(
                    watch_stmt,
                    1,
                    script_bytes.data(),
                    static_cast<int>(script_bytes.size()),
                    SQLITE_TRANSIENT);
                if (sqlite3_step(watch_stmt) == SQLITE_ROW) {
                    const char* watch_path = reinterpret_cast<const char*>(sqlite3_column_text(watch_stmt, 0));
                    if (watch_path && watch_path[0] != '\0') {
                        path_opt = std::string(watch_path);
                        WLOG_INFO("Recovered derivation path from watch_scripts: " + *path_opt +
                                  " for scriptPubKey: " + script_pubkey);
                    }
                }
                sqlite3_finalize(watch_stmt);
            }
        }
    }

    if (!path_opt) {
        // No HD derivation path found - check if this is an imported key
        // First, we need to find the address for this scriptPubKey to look it up in imported_keys
        WLOG_INFO("🔍 No HD derivation path for scriptPubKey: " + script_pubkey + " - checking imported keys");

        // Query imported_keys table by deriving address from scriptPubKey
        // For Taproot (witness v1), scriptPubKey format: 5120 + <32-byte pubkey>
        if (script_pubkey.length() == 68 && script_pubkey.substr(0, 4) == "5120") {
            // Extract the 32-byte output pubkey
            std::string output_pubkey_hex = script_pubkey.substr(4);

            // Encode as bech32m address
            std::vector<uint8_t> pubkey_bytes;
            for (size_t i = 0; i < output_pubkey_hex.length(); i += 2) {
                pubkey_bytes.push_back(std::stoi(output_pubkey_hex.substr(i, 2), nullptr, 16));
            }

            std::string address = AddressCodec::encodeP2TR(Network::MAIN, pubkey_bytes);
            WLOG_INFO("🔍 Derived address from scriptPubKey: " + address);

            // Now check imported_keys table
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT private_key_enc FROM imported_keys WHERE address = ?";

            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
                WLOG_INFO("🔍 Querying imported_keys for address: " + address);

                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    WLOG_INFO("🔍 Found row in imported_keys table");
                    const unsigned char* enc_key = sqlite3_column_text(stmt, 0);
                    if (enc_key) {
                        std::string key_storage(reinterpret_cast<const char*>(enc_key));
                        sqlite3_finalize(stmt);

                        // Decrypt or decode the private key
                        std::string privkey_hex;
                        if (!encryption_key_.empty()) {
                            // Key is encrypted, decrypt it
                            privkey_hex = decryptData(key_storage, encryption_key_);
                        } else {
                            // Key is stored as plaintext hex
                            privkey_hex = key_storage;
                        }

                        if (!privkey_hex.empty() && privkey_hex.length() == 64) {
                            // Convert hex to bytes
                            std::vector<uint8_t> privkey_bytes;
                            for (size_t i = 0; i < privkey_hex.length(); i += 2) {
                                privkey_bytes.push_back(std::stoi(privkey_hex.substr(i, 2), nullptr, 16));
                            }

                            // Cache and return
                            cachePrivateKey(script_pubkey, privkey_bytes);
                            WLOG_INFO("✅ Found imported private key for scriptPubKey: " + script_pubkey);
                            return privkey_bytes;
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }
        }

        // Fallback: look up the address registry (addresses table) by scriptPubKey.
        // Old wallets may have addresses registered here but not in address_derivation_paths.
        {
            sqlite3_stmt* addr_stmt = nullptr;
            const char* addr_sql = "SELECT type, account, change, idx FROM addresses WHERE script_pubkey = ? LIMIT 1";
            if (sqlite3_prepare_v2(db_, addr_sql, -1, &addr_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(addr_stmt, 1, script_pubkey.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(addr_stmt) == SQLITE_ROW) {
                    const char* type_str = reinterpret_cast<const char*>(sqlite3_column_text(addr_stmt, 0));
                    int account = sqlite3_column_int(addr_stmt, 1);
                    int change  = sqlite3_column_int(addr_stmt, 2);
                    int idx     = sqlite3_column_int(addr_stmt, 3);
                    int purpose = (type_str && std::string(type_str) == "p2tr") ? 86 : 84;

                    std::string recovered_path = "m/" + std::to_string(purpose) + "'/"
                        + std::to_string(dinero::consensus::DINERO_COIN_TYPE) + "'/"
                        + std::to_string(account) + "'/" + std::to_string(change) + "/" + std::to_string(idx);
                    sqlite3_finalize(addr_stmt);

                    WLOG_INFO("Recovered derivation path from address registry: " + recovered_path +
                              " for scriptPubKey: " + script_pubkey);

                    // Backfill address_derivation_paths for future lookups
                    sqlite3_stmt* ins_stmt = nullptr;
                    const char* ins_sql = "INSERT OR IGNORE INTO address_derivation_paths "
                        "(address, derivation_path, script_pubkey, account, change, address_index, created_at) "
                        "VALUES ((SELECT address FROM addresses WHERE script_pubkey = ? LIMIT 1), ?, ?, ?, ?, ?, datetime('now'))";
                    if (sqlite3_prepare_v2(db_, ins_sql, -1, &ins_stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(ins_stmt, 1, script_pubkey.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_text(ins_stmt, 2, recovered_path.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_text(ins_stmt, 3, script_pubkey.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int(ins_stmt, 4, account);
                        sqlite3_bind_int(ins_stmt, 5, change);
                        sqlite3_bind_int(ins_stmt, 6, idx);
                        sqlite3_step(ins_stmt);
                        sqlite3_finalize(ins_stmt);
                    }

                    path_opt = recovered_path;
                }
                if (!path_opt) sqlite3_finalize(addr_stmt);
            }
        }

        if (!path_opt) {
            WLOG_ERR("No derivation path or imported key found for scriptPubKey: " + script_pubkey);
            return std::nullopt;
        }
    }

    std::string derivation_path = *path_opt;
    WLOG_DEBUG("Deriving private key for scriptPubKey " + script_pubkey + " with path: " + derivation_path);

    // Watch-only namespaces are deliberately not BIP32 paths. Covenant
    // descriptors use a NUMS internal key and register
    // m/covenant/<profile>/<descriptor-id> solely for UTXO discovery. Trying
    // to parse that label with stoul both invents ownership and aborts
    // multi-input signing before an unrelated wallet-owned fee input can be
    // signed.
    if (derivation_path.rfind("m/covenant/", 0) == 0) {
        WLOG_DEBUG(
            "Covenant watch script has no wallet private key: " +
            script_pubkey);
        return std::nullopt;
    }

    // Parse derivation path (e.g., "m/84'/1448'/0'/0/0")
    // Expected format: m/purpose'/coin_type'/account'/change/address_index
    std::vector<uint32_t> path_components;
    size_t pos = 2; // Skip "m/"

    try {
        while (pos < derivation_path.length()) {
            size_t slash_pos = derivation_path.find('/', pos);
            std::string component = (slash_pos == std::string::npos)
                ? derivation_path.substr(pos)
                : derivation_path.substr(pos, slash_pos - pos);
            if (component.empty()) {
                return std::nullopt;
            }

            bool hardened = (component.back() == '\'');
            if (hardened) component.pop_back();
            if (component.empty() ||
                !std::all_of(
                    component.begin(),
                    component.end(),
                    [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                WLOG_ERR(
                    "Refusing non-BIP32 watch path for private-key "
                    "derivation: " + derivation_path);
                return std::nullopt;
            }

            uint32_t index = std::stoul(component);
            if (hardened) index |= 0x80000000; // Set hardened bit

            path_components.push_back(index);

            if (slash_pos == std::string::npos) break;
            pos = slash_pos + 1;
        }
    } catch (const std::exception& e) {
        WLOG_ERR(
            "Invalid BIP32 derivation path " + derivation_path +
            ": " + e.what());
        return std::nullopt;
    }

    // Derive key using BIP32Deriver (canonical engine with secure zeroization)

    // Unencrypted wallets should keep seed in memory, but recover defensively if it was cleared.
    if (master_seed_.empty() && !wallet_locked_) {
        auto seed_opt = loadMasterSeed("");
        if (seed_opt.has_value()) {
            master_seed_ = seed_opt.value();
        }
    }

    if (master_seed_.empty()) {
        return std::nullopt;
    }

    try {
        dinero::BIP32Deriver deriver(master_seed_.data(), master_seed_.size());
        for (uint32_t component : path_components) {
            if (component & 0x80000000) {
                deriver.deriveHardened(component & ~0x80000000);
            } else {
                deriver.deriveNormal(component);
            }
        }
        auto privkey = deriver.getPrivateKey();
        std::vector<uint8_t> private_key(privkey.begin(), privkey.end());

        // Cache for future use
        cachePrivateKey(script_pubkey, private_key);

        WLOG_INFO("Successfully derived private key for scriptPubKey: " + script_pubkey);
        return private_key;

    } catch (const std::exception& e) {
        WLOG_ERR("BIP32 derivation failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

bool WalletManager::hasSigningMaterialForScriptPubKey(const std::string& script_pubkey) const {
    if (script_pubkey.empty() || !db_ || !hasActiveWallet()) {
        return false;
    }

    if (getDerivationPath(script_pubkey).has_value()) {
        return true;
    }

    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT 1 FROM addresses WHERE script_pubkey = ? LIMIT 1";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, script_pubkey.c_str(), -1, SQLITE_STATIC);
            const bool found = sqlite3_step(stmt) == SQLITE_ROW;
            sqlite3_finalize(stmt);
            if (found) {
                return true;
            }
        }
    }

    // Phase 10: v7 P2MR (witness v3) ownership is tracked in watch_scripts
    // rather than addresses/address_derivation_paths — it's registered by
    // wallet.getnewp2mraddress and keyed by the 34-byte scriptPubKey blob
    // (0x53 0x20 || merkle_root). Without this branch, P2MR UTXOs would
    // flow through listunspent with solvable=false → spendable=false, and
    // coin selection would never pick them.
    if (script_pubkey.length() == 68 && script_pubkey.rfind("5320", 0) == 0) {
        std::vector<uint8_t> spk_bytes;
        spk_bytes.reserve(34);
        for (size_t i = 0; i + 1 < script_pubkey.length(); i += 2) {
            spk_bytes.push_back(static_cast<uint8_t>(
                std::stoi(script_pubkey.substr(i, 2), nullptr, 16)));
        }
        if (getWatchScriptPath(spk_bytes).has_value()) {
            return true;
        }
    }

    // Imported keys are stored by address, so reconstruct the address for
    // supported script shapes and probe imported_keys directly.
    if (script_pubkey.length() == 68 && script_pubkey.rfind("5120", 0) == 0) {
        try {
            std::vector<uint8_t> pubkey_bytes;
            pubkey_bytes.reserve(32);
            for (size_t i = 4; i < script_pubkey.length(); i += 2) {
                pubkey_bytes.push_back(static_cast<uint8_t>(std::stoi(script_pubkey.substr(i, 2), nullptr, 16)));
            }

            const std::string address = AddressCodec::encodeP2TR(Network::MAIN, pubkey_bytes);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT 1 FROM imported_keys WHERE address = ? LIMIT 1";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
                const bool found = sqlite3_step(stmt) == SQLITE_ROW;
                sqlite3_finalize(stmt);
                if (found) {
                    return true;
                }
            }
        } catch (const std::exception&) {
            return false;
        }
    }

    return false;
}

std::optional<std::string> WalletManager::getDerivationPath(const std::string& script_pubkey) const {
    if (!db_) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    // ⚠️ OWNERSHIP LOGIC - Uses scriptPubKey (consensus data), NOT address (display string)
    const char* sql = "SELECT derivation_path FROM address_derivation_paths WHERE script_pubkey = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        WLOG_ERR("Failed to prepare derivation path query");
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, script_pubkey.c_str(), -1, SQLITE_STATIC);

    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (path) {
            result = std::string(path);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<std::string> WalletManager::getWatchScriptPath(const std::vector<uint8_t>& script_pubkey) const {
    if (!db_ || script_pubkey.empty()) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT path FROM watch_scripts WHERE script_pubkey = ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_blob(stmt, 1, script_pubkey.data(), static_cast<int>(script_pubkey.size()), SQLITE_TRANSIENT);
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (path && path[0] != '\0') result = std::string(path);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<std::string> WalletManager::getScriptPubKeyForAddress(const std::string& address) const {
    if (!db_) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    // ⚠️ TEMPORARY BRIDGE - This function exists only to support migration from address-based to scriptPubKey-based lookups
    const char* sql = "SELECT script_pubkey FROM addresses WHERE address = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        WLOG_ERR("Failed to prepare scriptPubKey query");
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);

    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* spk = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (spk) {
            result = std::string(spk);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

std::string WalletManager::getPrivateKeyForPath(const std::string& derivation_path) {
    WLOG_INFO("🔑 getPrivateKeyForPath() called with path: " + derivation_path);

    // ═══════════════════════════════════════════════════════════════════════
    // Week 1 Day 5: Refactored to use DerivePrivateKey (descriptor wallet)
    // ═══════════════════════════════════════════════════════════════════════

    // Check if wallet is active and unlocked
    if (!hasActiveWallet()) {
        WLOG_ERR("🔑 ❌ No active wallet");
        return "";
    }

    WLOG_INFO("🔑 Wallet state: locked=" + std::to_string(wallet_locked_) +
              ", encrypted=" + std::to_string(wallet_encrypted_));

    if (wallet_locked_) {
        WLOG_ERR("🔑 ❌ Wallet is LOCKED - cannot access private keys");
        return "";
    }

    // Unencrypted wallets should keep seed in memory, but recover defensively if it was cleared.
    if (master_seed_.empty() && !wallet_locked_) {
        auto seed_opt = loadMasterSeed("");
        if (seed_opt.has_value()) {
            master_seed_ = seed_opt.value();
            WLOG_INFO("🔑 Recovered master seed in-memory for unencrypted wallet path derivation");
        }
    }

    if (derivation_path.empty() || derivation_path.substr(0, 2) != "m/") {
        WLOG_ERR("🔑 ❌ Invalid derivation path: " + derivation_path);
        return "";
    }
    try {
        dinero::wallet::RejectRetiredLegacyCoinTypeText(derivation_path, "wallet private-key derivation");
    } catch (const std::exception& e) {
        WLOG_ERR(std::string("🔑 ❌ ") + e.what());
        return "";
    }

    WLOG_DEBUG("🔑 Deriving private key for path: " + derivation_path);

    // Parse derivation path to KeyOriginInfo
    auto origin_opt = wallet::KeyOriginInfo::parsePathString(derivation_path);
    if (!origin_opt.has_value()) {
        WLOG_ERR("🔑 ❌ Failed to parse derivation path: " + derivation_path);
        return "";
    }

    // Derive private key using descriptor wallet method
    auto privkey_opt = DerivePrivateKey(origin_opt.value());
    if (!privkey_opt.has_value()) {
        WLOG_ERR("🔑 ❌ Failed to derive private key for path: " + derivation_path);
        return "";
    }

    std::vector<uint8_t> final_privkey = privkey_opt.value();

    // CRITICAL BIP341 FIX: For Taproot (BIP86), DO NOT tweak the private key
    // Taproot key-path spending uses the INTERNAL (untweaked) private key for signing
    // The tweaking is applied to the PUBLIC key to create the output key
    // Signature is verified against the tweaked output key, but made with internal privkey
    //
    // Previous implementation (WRONG): Applied TapTweak to private key
    // Current implementation (CORRECT): Return internal private key as-is
    //
    // This fixes the "Could not retrieve private keys for signing" bug in Phase 4C-lite

    uint32_t purpose = origin_opt->getPurpose();
    bool is_taproot = (purpose == 86);

    if (is_taproot) {
        WLOG_INFO("🔑 ✅ Returning INTERNAL (untweaked) private key for Taproot path: " + derivation_path);
        WLOG_INFO("🔑    BIP341: Signature uses internal key, verified against tweaked output key");
    }

    // Extract private key (32 bytes) and convert to hex
    std::string hex_key;
    hex_key.reserve(64);
    static const char* hex_chars = "0123456789abcdef";
    for (uint8_t byte : final_privkey) {
        hex_key += hex_chars[(byte >> 4) & 0xF];
        hex_key += hex_chars[byte & 0xF];
    }

    WLOG_INFO("🔑 ✅ Successfully derived private key for path: " + derivation_path);

    // Securely erase the private key
    OPENSSL_cleanse(final_privkey.data(), final_privkey.size());

    return hex_key;
}

// ============================================================================
// WIF (Wallet Import Format) Implementation
// ============================================================================

std::vector<uint8_t> WalletManager::decodeWIF(const std::string& wif) {
    // Decode Base58Check
    std::vector<uint8_t> decoded;
    if (!dinero::Address::decodeBase58Check(wif, decoded)) {
        WLOG_ERR("Invalid WIF: Base58Check decode failed");
        return {};
    }

    // Validate length: 33 bytes (uncompressed) or 34 bytes (compressed)
    if (decoded.size() != 33 && decoded.size() != 34) {
        WLOG_ERR("Invalid WIF length: " + std::to_string(decoded.size()));
        return {};
    }

    // Check prefix
    uint8_t prefix = decoded[0];
    bool valid_prefix = (prefix == 0x9E) ||  // Dinero mainnet
                        (prefix == 0x80) ||  // Bitcoin mainnet
                        (prefix == 0xEF);    // Testnet
    if (!valid_prefix) {
        WLOG_ERR("Invalid WIF prefix: 0x" + std::to_string(prefix));
        return {};
    }

    // Check compression suffix if present
    if (decoded.size() == 34) {
        if (decoded[33] != 0x01) {
            WLOG_ERR("Invalid compressed WIF suffix");
            return {};
        }
    }

    // Extract 32-byte private key (skip prefix byte)
    std::vector<uint8_t> privkey(decoded.begin() + 1, decoded.begin() + 33);
    return privkey;
}

std::string WalletManager::encodeWIF(const std::vector<uint8_t>& privkey, bool compressed, bool testnet) {
    if (privkey.size() != 32) {
        WLOG_ERR("Invalid private key length for WIF encoding");
        return "";
    }

    // Build payload: prefix + privkey + optional compression byte
    std::vector<uint8_t> payload;
    payload.reserve(compressed ? 34 : 33);

    // Prefix: 0x9E for Dinero mainnet, 0xEF for testnet
    payload.push_back(testnet ? 0xEF : 0x9E);

    // 32-byte private key
    payload.insert(payload.end(), privkey.begin(), privkey.end());

    // Compression suffix
    if (compressed) {
        payload.push_back(0x01);
    }

    // Base58Check encode
    return dinero::Address::encodeBase58Check(payload);
}

bool WalletManager::validateWIF(const std::string& wif, bool& is_compressed, bool& is_testnet) {
    std::vector<uint8_t> decoded;
    if (!dinero::Address::decodeBase58Check(wif, decoded)) {
        return false;
    }

    if (decoded.size() != 33 && decoded.size() != 34) {
        return false;
    }

    uint8_t prefix = decoded[0];
    is_testnet = (prefix == 0xEF);
    is_compressed = (decoded.size() == 34 && decoded[33] == 0x01);

    // Validate prefix
    return (prefix == 0x9E || prefix == 0x80 || prefix == 0xEF);
}

std::string WalletManager::importPrivateKey(const std::vector<uint8_t>& privkey, const std::string& label) {
    if (privkey.size() != 32) {
        WLOG_ERR("Invalid private key length");
        return "";
    }

    if (!hasActiveWallet()) {
        WLOG_ERR("No active wallet for import");
        return "";
    }

    try {
        // Derive public key from private key using secp256k1
        secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
        if (!ctx) {
            WLOG_ERR("Failed to create secp256k1 context");
            return "";
        }

        // Verify private key is valid
        if (!secp256k1_ec_seckey_verify(ctx, privkey.data())) {
            secp256k1_context_destroy(ctx);
            WLOG_ERR("Invalid private key");
            return "";
        }

        // Create public key
        secp256k1_pubkey pubkey;
        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privkey.data())) {
            secp256k1_context_destroy(ctx);
            WLOG_ERR("Failed to create public key");
            return "";
        }

        // Dinero uses Taproot from genesis - create P2TR address
        // For BIP341 Taproot, we need to apply TapTweak to get the output pubkey

        // Step 1: Get internal x-only pubkey
        secp256k1_xonly_pubkey internal_pubkey;
        int pk_parity;
        if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &internal_pubkey, &pk_parity, &pubkey)) {
            secp256k1_context_destroy(ctx);
            WLOG_ERR("Failed to extract x-only pubkey");
            return "";
        }

        // Serialize internal pubkey for TapTweak computation
        std::vector<uint8_t> internal_xonly(32);
        secp256k1_xonly_pubkey_serialize(ctx, internal_xonly.data(), &internal_pubkey);

        // Step 2: Compute TapTweak
        // NOTE: Using SHA256(internal_key || 0x00) for BIP86 key-path-only tweak
        // (empty merkle root). This matches BIP341 key-spend-only convention.
        unsigned char tweak_data[33];
        std::memcpy(tweak_data, internal_xonly.data(), 32);
        tweak_data[32] = 0x00;  // Empty merkle root

        unsigned char tweak[32];
        SHA256(tweak_data, 33, tweak);

        // Step 3: Apply tweak: output_key = internal_key + tweak*G
        secp256k1_pubkey tweaked_pubkey;
        if (!secp256k1_xonly_pubkey_tweak_add(ctx, &tweaked_pubkey, &internal_pubkey, tweak)) {
            secp256k1_context_destroy(ctx);
            WLOG_ERR("Failed to apply TapTweak");
            return "";
        }

        // Step 4: Extract output x-only pubkey
        secp256k1_xonly_pubkey output_pubkey;
        if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_pubkey, nullptr, &tweaked_pubkey)) {
            secp256k1_context_destroy(ctx);
            WLOG_ERR("Failed to extract output x-only pubkey");
            return "";
        }

        // Serialize output pubkey (32 bytes)
        std::vector<uint8_t> x_only_pubkey(32);
        secp256k1_xonly_pubkey_serialize(ctx, x_only_pubkey.data(), &output_pubkey);
        secp256k1_context_destroy(ctx);

        // Debug: Log the computed pubkeys
        std::ostringstream debug_stream;
        debug_stream << "TapTweak Debug:\n";
        debug_stream << "  Internal pubkey: ";
        for (auto b : internal_xonly) {
            debug_stream << std::hex << std::setfill('0') << std::setw(2) << (int)b;
        }
        debug_stream << "\n  Output pubkey:   ";
        for (auto b : x_only_pubkey) {
            debug_stream << std::hex << std::setfill('0') << std::setw(2) << (int)b;
        }
        debug_stream << "\n  Expected:        ca062bff883f6d1511a72a78a09f3a17cb6734f9ce8543faa66f7e380d58d1b7";
        WLOG_INFO(debug_stream.str());

        // Create bech32m address (P2TR - witness version 1) using AddressCodec
        std::string address = AddressCodec::encodeP2TR(Network::MAIN, x_only_pubkey);

        if (address.empty()) {
            WLOG_ERR("Failed to encode address");
            return "";
        }

        // Store in imported_keys table
        const char* sql = R"(
            INSERT OR REPLACE INTO imported_keys (address, private_key_enc, label, created_at)
            VALUES (?, ?, ?, datetime('now'))
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            WLOG_ERR("Failed to prepare import statement");
            return "";
        }

        // For now, store the key encrypted with wallet's encryption key if available
        // Otherwise store as hex (not recommended for production)
        std::string key_storage;
        if (!encryption_key_.empty()) {
            key_storage = encryptData(std::string(privkey.begin(), privkey.end()), encryption_key_);
        } else {
            // Hex encode for unencrypted wallets
            static const char* hex_chars = "0123456789abcdef";
            key_storage.reserve(64);
            for (uint8_t byte : privkey) {
                key_storage += hex_chars[(byte >> 4) & 0xF];
                key_storage += hex_chars[byte & 0xF];
            }
        }

        sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, key_storage.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, label.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            WLOG_ERR("Failed to store imported key: " + std::string(sqlite3_errmsg(db_)));
            sqlite3_finalize(stmt);
            return "";
        }
        sqlite3_finalize(stmt);

        // Add to address book via HD address method (account -1 = imported)
        addHDAddress(address, -1, 0, 0, label.empty() ? "imported" : label);

        // Cache the key for immediate use
        cachePrivateKey(address, privkey);

        WLOG_INFO("Successfully imported private key for address: " + address);
        return address;

    } catch (const std::exception& e) {
        WLOG_ERR("Import failed: " + std::string(e.what()));
        return "";
    }
}

void WalletManager::cachePrivateKey(const std::string& address, const std::vector<uint8_t>& key) {
    // Store private key in cache (in memory only while wallet is unlocked)
    private_key_cache_[address] = key;
    WLOG_DEBUG("Cached private key for address: " + address);
}

void WalletManager::clearPrivateKeyCache() {
    // Securely clear all cached private keys
    for (auto& pair : private_key_cache_) {
        secureClearBytes(pair.second);
    }
    private_key_cache_.clear();
    WLOG_INFO("Cleared private key cache from memory");
}

bool WalletManager::storeMasterSeed(const std::vector<uint8_t>& seed,
                                    const std::string& passphrase,
                                    bool reset_address_state) {
    if (!db_ || current_wallet_id_ < 0) {
        WLOG_ERR("No active wallet to store master seed");
        return false;
    }

    if (seed.size() != 64) {
        WLOG_ERR("Invalid seed size: " + std::to_string(seed.size()) + " (expected 64 bytes)");
        return false;
    }

    // Re-importing the active recovery phrase is an idempotent wallet bind,
    // not a wallet replacement. In particular, embedded/mobile clients bind
    // on every process start and may explicitly skip address derivation after
    // the first successful bind. Clearing address state here would therefore
    // erase watch_scripts and make the otherwise valid retry unusable.
    if (master_seed_.empty() && !wallet_locked_) {
        auto existing_seed = loadMasterSeed("");
        if (existing_seed.has_value()) {
            master_seed_ = std::move(existing_seed.value());
        }
    }
    const bool replaces_wallet_identity =
        reset_address_state && !ConstantTimeEqual(seed, master_seed_);

    // ═══════════════════════════════════════════════════════════════════════
    // Optional address-state reset
    // ═══════════════════════════════════════════════════════════════════════
    // Required when replacing the wallet seed (restore/import flows), but
    // must NOT run during wallet encryption, where we only re-encrypt
    // the same seed.
    // ═══════════════════════════════════════════════════════════════════════

    if (replaces_wallet_identity) {
        WLOG_INFO("Clearing address tables for new HD seed import...");

        // Clear addresses table (resets derivation index to 0)
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db_, "DELETE FROM addresses", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            WLOG_WARN("Failed to clear addresses table: " + std::string(err_msg ? err_msg : "unknown error"));
            if (err_msg) sqlite3_free(err_msg);
        }

        // Clear address derivation paths
        rc = sqlite3_exec(db_, "DELETE FROM address_derivation_paths", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            WLOG_WARN("Failed to clear address_derivation_paths: " + std::string(err_msg ? err_msg : "unknown error"));
            if (err_msg) sqlite3_free(err_msg);
        }

        // Clear HD address book entries
        rc = sqlite3_exec(db_, "DELETE FROM hd_address_book", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            // Table may not exist in older schemas, ignore error
            if (err_msg) sqlite3_free(err_msg);
        }

        // Clear UTXO entries (they belong to old seed's addresses)
        rc = sqlite3_exec(db_, "DELETE FROM utxos", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            WLOG_WARN("Failed to clear utxos: " + std::string(err_msg ? err_msg : "unknown error"));
            if (err_msg) sqlite3_free(err_msg);
        }

        // Clear watch_scripts (they belong to old seed's scriptPubKeys)
        rc = sqlite3_exec(db_, "DELETE FROM watch_scripts", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            WLOG_WARN("Failed to clear watch_scripts: " + std::string(err_msg ? err_msg : "unknown error"));
            if (err_msg) sqlite3_free(err_msg);
        }

        // Clear taproot_key_mapping (output/internal pubkey pairs belong to old seed)
        rc = sqlite3_exec(db_, "DELETE FROM taproot_key_mapping", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            // Table may not exist in older schemas, ignore error
            if (err_msg) sqlite3_free(err_msg);
        }

        // Clear transactions (they belong to previous wallet's addresses, not the new seed)
        rc = sqlite3_exec(db_, "DELETE FROM transactions", nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            WLOG_WARN("Failed to clear transactions: " + std::string(err_msg ? err_msg : "unknown error"));
            if (err_msg) sqlite3_free(err_msg);
        }

        WLOG_INFO("✅ Address, watch_script, and transaction tables cleared - derivation will start from index 0");
    }

    // === STEP 1: Generate random salt and nonce ===
    constexpr size_t SALT_SIZE = 32;
    constexpr size_t NONCE_SIZE = 12;
    constexpr size_t TAG_SIZE = 16;
    constexpr uint32_t PBKDF2_ITERATIONS = 600000;

    std::vector<uint8_t> salt(SALT_SIZE);
    std::vector<uint8_t> nonce(NONCE_SIZE);

    if (RAND_bytes(salt.data(), SALT_SIZE) != 1) {
        WLOG_ERR("Failed to generate random salt");
        return false;
    }

    if (RAND_bytes(nonce.data(), NONCE_SIZE) != 1) {
        WLOG_ERR("Failed to generate random nonce");
        return false;
    }

    // === STEP 2: Derive encryption key from passphrase using PBKDF2-HMAC-SHA512 ===
    uint8_t derived_key[64];  // PBKDF2-SHA512 gives 64 bytes, we use first 32 for AES-256
    dinero::crypto::PBKDF2_HMAC_SHA512(
        reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size(),
        salt.data(), salt.size(),
        PBKDF2_ITERATIONS,
        derived_key, sizeof(derived_key)
    );

    // Use first 32 bytes as AES-256 key
    uint8_t aes_key[32];
    std::memcpy(aes_key, derived_key, 32);

    // === STEP 3: Encrypt seed with AES-256-GCM ===
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        OPENSSL_cleanse(derived_key, sizeof(derived_key));
        OPENSSL_cleanse(aes_key, sizeof(aes_key));
        WLOG_ERR("Failed to create cipher context");
        return false;
    }

    bool success = false;
    std::vector<uint8_t> ciphertext(seed.size());
    std::vector<uint8_t> tag(TAG_SIZE);

    do {
        // Initialize AES-256-GCM encryption
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, aes_key, nonce.data()) != 1) {
            WLOG_ERR("Failed to initialize AES-256-GCM encryption");
            break;
        }

        // Encrypt the seed
        int len = 0;
        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, seed.data(), seed.size()) != 1) {
            WLOG_ERR("Failed to encrypt seed");
            break;
        }

        int ciphertext_len = len;

        // Finalize encryption
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
            WLOG_ERR("Failed to finalize encryption");
            break;
        }

        ciphertext_len += len;

        // Get authentication tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag.data()) != 1) {
            WLOG_ERR("Failed to get GCM tag");
            break;
        }

        // === STEP 4: Store encrypted_seed + salt + nonce + tag in database ===
        // Format: salt(32) + nonce(12) + ciphertext(64) + tag(16) = 124 bytes total
        std::vector<uint8_t> encrypted_blob;
        encrypted_blob.reserve(SALT_SIZE + NONCE_SIZE + ciphertext_len + TAG_SIZE);
        encrypted_blob.insert(encrypted_blob.end(), salt.begin(), salt.end());
        encrypted_blob.insert(encrypted_blob.end(), nonce.begin(), nonce.end());
        encrypted_blob.insert(encrypted_blob.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);
        encrypted_blob.insert(encrypted_blob.end(), tag.begin(), tag.end());

        // Insert or replace into hd_seeds table (per-wallet DB uses id=1)
        sqlite3_stmt* stmt = nullptr;
        const char* sql = R"(
            INSERT OR REPLACE INTO hd_seeds (id, encrypted_seed, salt, coin_type, encryption_version, created_at)
            VALUES (1, ?, ?, ?, 2, strftime('%s','now'))
        )";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            WLOG_ERR("Failed to prepare statement for storing seed: " + std::string(sqlite3_errmsg(db_)));
            break;
        }

        sqlite3_bind_blob(stmt, 1, encrypted_blob.data(), encrypted_blob.size(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, salt.data(), salt.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, static_cast<int>(dinero::consensus::DINERO_COIN_TYPE));

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            WLOG_ERR("Failed to store encrypted seed in database");
            break;
        }

        WLOG_INFO("Successfully stored encrypted HD wallet seed (encryption_version=2, PBKDF2@600K)");

        // Update encryption_metadata with actual seed KDF params
        {
            sqlite3_stmt* meta_stmt = nullptr;
            const char* meta_sql = R"(
                INSERT OR REPLACE INTO encryption_metadata (
                    id, encrypted, kdf, kdf_iterations, cipher, salt, created_at, updated_at
                )
                VALUES (1, COALESCE((SELECT encrypted FROM encryption_metadata WHERE id = 1), 0),
                        'pbkdf2-hmac-sha512', 600000, 'AES-256-GCM', ?,
                        COALESCE((SELECT created_at FROM encryption_metadata WHERE id = 1), strftime('%s','now')),
                        strftime('%s','now'))
            )";
            if (sqlite3_prepare_v2(db_, meta_sql, -1, &meta_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_blob(meta_stmt, 1, salt.data(), salt.size(), SQLITE_TRANSIENT);
                sqlite3_step(meta_stmt);
                sqlite3_finalize(meta_stmt);
            }
        }

        // Any operation that replaces the seed invalidates the old mnemonic
        // binding. Clear it after the seed write succeeds; encryption passes
        // reset_address_state=false because they preserve the same identity.
        if (replaces_wallet_identity) {
            try {
                setSetting(kBip39RecoverySetting, "");
                setSetting(kBip39BackupAcknowledgedSetting, "0");
            } catch (const std::exception& e) {
                WLOG_ERR("Failed to invalidate old BIP39 recovery record: " +
                         std::string(e.what()));
                break;
            }
        }

        // ✅ CRITICAL FIX: Set master_seed_ in memory so getNewAddress works immediately
        // Without this, wallet.restore can't generate addresses after storing seed
        master_seed_ = seed;

        success = true;

    } while (false);

    // Cleanup
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(derived_key, sizeof(derived_key));
    OPENSSL_cleanse(aes_key, sizeof(aes_key));

    return success;
}

bool WalletManager::storeAuthoritativeBip39Mnemonic(
    const std::string& mnemonic,
    const std::string& bip39_passphrase,
    std::string* error_out) {
    if (!db_ || current_wallet_id_ < 0) {
        SetRecoveryError(error_out, "no active wallet");
        return false;
    }
    if (master_seed_.size() != 64) {
        SetRecoveryError(error_out, "active wallet seed is unavailable; unlock the wallet first");
        return false;
    }
    if (!bip39::ValidateMnemonic(mnemonic)) {
        SetRecoveryError(error_out, "mnemonic is not valid BIP39 recovery material");
        return false;
    }

    std::vector<uint8_t> entropy;
    std::vector<uint8_t> derived_seed;
    if (!bip39::MnemonicToEntropy(mnemonic, entropy) || entropy.empty() ||
        !bip39::MnemonicToSeed(mnemonic, bip39_passphrase, derived_seed)) {
        SetRecoveryError(error_out, "failed to derive BIP39 recovery material");
        secureClearBytes(entropy);
        secureClearBytes(derived_seed);
        return false;
    }
    if (!ConstantTimeEqual(derived_seed, master_seed_)) {
        SetRecoveryError(error_out, "mnemonic/passphrase does not reproduce the active wallet seed");
        secureClearBytes(entropy);
        secureClearBytes(derived_seed);
        return false;
    }

    std::array<uint8_t, 32> key{};
    std::vector<uint8_t> plaintext;
    try {
        key = DeriveBip39RecoveryKey(master_seed_);
        std::vector<uint8_t> nonce(kBip39RecoveryNonceSize);
        if (RAND_bytes(nonce.data(), nonce.size()) != 1) {
            OPENSSL_cleanse(key.data(), key.size());
            SetRecoveryError(error_out, "failed to generate recovery-record nonce");
            secureClearBytes(entropy);
            secureClearBytes(derived_seed);
            return false;
        }

        plaintext.reserve(1 + entropy.size());
        plaintext.push_back(bip39_passphrase.empty() ? 0 : kBip39PassphraseRequired);
        plaintext.insert(plaintext.end(), entropy.begin(), entropy.end());
        std::vector<uint8_t> ciphertext = crypto::encryptAesGcm(plaintext, key, nonce);

        std::vector<uint8_t> record;
        record.reserve(1 + nonce.size() + ciphertext.size());
        record.push_back(kBip39RecoveryRecordVersion);
        record.insert(record.end(), nonce.begin(), nonce.end());
        record.insert(record.end(), ciphertext.begin(), ciphertext.end());

        setSetting(kBip39RecoverySetting, util::hex(record));
        setSetting(kBip39BackupAcknowledgedSetting, "0");

        OPENSSL_cleanse(key.data(), key.size());
        secureClearBytes(plaintext);
        secureClearBytes(entropy);
        secureClearBytes(derived_seed);
        return true;
    } catch (const std::exception& e) {
        SetRecoveryError(error_out, std::string("failed to persist recovery record: ") + e.what());
        OPENSSL_cleanse(key.data(), key.size());
        secureClearBytes(plaintext);
        secureClearBytes(entropy);
        secureClearBytes(derived_seed);
        return false;
    }
}

std::optional<Bip39RecoveryMaterial> WalletManager::loadAuthoritativeBip39Mnemonic(
    std::string* error_out) const {
    if (!db_ || current_wallet_id_ < 0) {
        SetRecoveryError(error_out, "no active wallet");
        return std::nullopt;
    }

    const std::string encoded = getSetting(kBip39RecoverySetting);
    if (encoded.empty()) {
        SetRecoveryError(error_out,
                         "no authoritative mnemonic exists for this wallet; it predates mnemonic-backed creation or was created from a raw seed");
        return std::nullopt;
    }
    if (master_seed_.size() != 64) {
        SetRecoveryError(error_out, "wallet is locked; unlock it before exporting recovery material");
        return std::nullopt;
    }

    std::vector<unsigned char> record;
    if (!util::unhex(encoded, record) ||
        record.size() < 1 + kBip39RecoveryNonceSize + 16 + 2 ||
        record[0] != kBip39RecoveryRecordVersion) {
        SetRecoveryError(error_out, "authoritative mnemonic record is corrupt or unsupported");
        secureClearBytes(record);
        return std::nullopt;
    }

    std::array<uint8_t, 32> key{};
    std::vector<uint8_t> plaintext;
    try {
        std::vector<uint8_t> nonce(record.begin() + 1,
                                   record.begin() + 1 + kBip39RecoveryNonceSize);
        std::vector<uint8_t> ciphertext(record.begin() + 1 + kBip39RecoveryNonceSize,
                                        record.end());
        key = DeriveBip39RecoveryKey(master_seed_);
        plaintext = crypto::decryptAesGcm(ciphertext, key, nonce);
        OPENSSL_cleanse(key.data(), key.size());
        secureClearBytes(record);

        if (plaintext.size() < 2 || (plaintext[0] & ~kBip39PassphraseRequired) != 0) {
            SetRecoveryError(error_out, "authoritative mnemonic record has invalid flags or entropy");
            secureClearBytes(plaintext);
            return std::nullopt;
        }

        const bool passphrase_required =
            (plaintext[0] & kBip39PassphraseRequired) != 0;
        std::vector<uint8_t> entropy(plaintext.begin() + 1, plaintext.end());
        const std::string mnemonic = bip39::EntropyToMnemonic(entropy.data(), entropy.size());
        secureClearBytes(entropy);
        secureClearBytes(plaintext);
        if (mnemonic.empty() || !bip39::ValidateMnemonic(mnemonic)) {
            SetRecoveryError(error_out, "authoritative mnemonic record does not contain valid BIP39 entropy");
            return std::nullopt;
        }

        // Without a BIP39 passphrase, independently re-derive and compare the
        // seed at every export. With a passphrase, AES-GCM authentication under
        // a key derived from the active seed preserves the creation-time binding;
        // the passphrase itself is deliberately never persisted.
        if (!passphrase_required) {
            std::vector<uint8_t> derived_seed;
            if (!bip39::MnemonicToSeed(mnemonic, "", derived_seed) ||
                !ConstantTimeEqual(derived_seed, master_seed_)) {
                SetRecoveryError(error_out, "authoritative mnemonic no longer matches the active wallet seed");
                secureClearBytes(derived_seed);
                return std::nullopt;
            }
            secureClearBytes(derived_seed);
        }

        Bip39RecoveryMaterial material;
        material.mnemonic = mnemonic;
        material.passphrase_required = passphrase_required;
        material.backup_acknowledged =
            getSetting(kBip39BackupAcknowledgedSetting) == "1";
        return material;
    } catch (const std::exception& e) {
        SetRecoveryError(error_out, std::string("authoritative mnemonic authentication failed: ") + e.what());
        OPENSSL_cleanse(key.data(), key.size());
        secureClearBytes(plaintext);
        secureClearBytes(record);
        return std::nullopt;
    }
}

bool WalletManager::hasAuthoritativeBip39Mnemonic() const {
    return !getSetting(kBip39RecoverySetting).empty();
}

bool WalletManager::acknowledgeBip39Backup(const std::string& mnemonic,
                                           bool passphrase_backed_up,
                                           std::string* error_out) {
    auto material = loadAuthoritativeBip39Mnemonic(error_out);
    if (!material.has_value()) {
        return false;
    }
    if (material->mnemonic.size() != mnemonic.size() ||
        CRYPTO_memcmp(material->mnemonic.data(), mnemonic.data(), mnemonic.size()) != 0) {
        SetRecoveryError(error_out, "mnemonic does not match the active wallet recovery record");
        OPENSSL_cleanse(material->mnemonic.data(), material->mnemonic.size());
        return false;
    }
    if (material->passphrase_required && !passphrase_backed_up) {
        SetRecoveryError(error_out,
                         "this wallet requires its separate BIP39 passphrase; confirm that it is backed up too");
        OPENSSL_cleanse(material->mnemonic.data(), material->mnemonic.size());
        return false;
    }

    try {
        setSetting(kBip39BackupAcknowledgedSetting, "1");
        OPENSSL_cleanse(material->mnemonic.data(), material->mnemonic.size());
        return true;
    } catch (const std::exception& e) {
        SetRecoveryError(error_out, std::string("failed to record backup acknowledgment: ") + e.what());
        OPENSSL_cleanse(material->mnemonic.data(), material->mnemonic.size());
        return false;
    }
}

std::optional<std::vector<uint8_t>> WalletManager::loadMasterSeed(const std::string& passphrase) {
    if (!db_ || current_wallet_id_ < 0) {
        WLOG_ERR("No active wallet to load master seed from");
        return std::nullopt;
    }

    // === STEP 1: Load encrypted seed + encryption_version from database ===
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT encrypted_seed, encryption_version FROM hd_seeds WHERE id = 1";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    const void* blob_data = sqlite3_column_blob(stmt, 0);
    int blob_size = sqlite3_column_bytes(stmt, 0);
    int encryption_version = sqlite3_column_int(stmt, 1); // 0 if NULL

    if (!blob_data || blob_size < 92) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    std::vector<uint8_t> encrypted_blob(static_cast<const uint8_t*>(blob_data),
                                        static_cast<const uint8_t*>(blob_data) + blob_size);
    sqlite3_finalize(stmt);

    // === STEP 2: Extract components from encrypted blob ===
    // Format: salt(32) + nonce(12) + ciphertext(64) + tag(16) = 124 bytes
    constexpr size_t SALT_SIZE = 32;
    constexpr size_t NONCE_SIZE = 12;
    constexpr size_t TAG_SIZE = 16;

    // Version-dispatched iteration count:
    //   Version 2 (current) = 600,000 PBKDF2-HMAC-SHA512 iterations
    //   Version 1 (legacy)  = 100,000 PBKDF2-HMAC-SHA512 iterations
    //   Fallback: try both if version is unknown (0 or missing)
    std::vector<uint32_t> iteration_candidates;
    if (encryption_version == 2) {
        iteration_candidates = {600000};
    } else if (encryption_version == 1) {
        iteration_candidates = {100000};
    } else {
        // Unknown version or NULL — try all known candidates (backward compat)
        iteration_candidates = {600000, 100000};
        WLOG_WARN("Unknown seed encryption_version=" + std::to_string(encryption_version) +
                  ", trying all known iteration counts");
    }

    std::vector<uint8_t> salt(encrypted_blob.begin(), encrypted_blob.begin() + SALT_SIZE);
    std::vector<uint8_t> nonce(encrypted_blob.begin() + SALT_SIZE,
                                encrypted_blob.begin() + SALT_SIZE + NONCE_SIZE);

    size_t ciphertext_len = encrypted_blob.size() - SALT_SIZE - NONCE_SIZE - TAG_SIZE;
    std::vector<uint8_t> ciphertext(encrypted_blob.begin() + SALT_SIZE + NONCE_SIZE,
                                    encrypted_blob.begin() + SALT_SIZE + NONCE_SIZE + ciphertext_len);
    std::vector<uint8_t> tag(encrypted_blob.end() - TAG_SIZE, encrypted_blob.end());

    // === STEP 3: Decrypt with canonical PBKDF2-HMAC-SHA512 ===
    for (uint32_t iterations : iteration_candidates) {
        uint8_t derived_key[64];
        dinero::crypto::PBKDF2_HMAC_SHA512(
            reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size(),
            salt.data(), salt.size(),
            iterations,
            derived_key, sizeof(derived_key));

        uint8_t aes_key[32];
        std::memcpy(aes_key, derived_key, 32);

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            OPENSSL_cleanse(derived_key, sizeof(derived_key));
            OPENSSL_cleanse(aes_key, sizeof(aes_key));
            continue;
        }

        std::vector<uint8_t> plaintext(ciphertext_len);
        bool decrypted = false;

        do {
            if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, aes_key, nonce.data()) != 1) break;

            int len = 0;
            if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext_len) != 1) break;
            int plaintext_len = len;

            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                                    const_cast<uint8_t*>(tag.data())) != 1) break;

            if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) break;

            plaintext_len += len;
            plaintext.resize(plaintext_len);

            if (plaintext.size() == 64) {
                decrypted = true;
            }
        } while (false);

        EVP_CIPHER_CTX_free(ctx);
        OPENSSL_cleanse(derived_key, sizeof(derived_key));
        OPENSSL_cleanse(aes_key, sizeof(aes_key));

        if (decrypted) {
            return plaintext;
        }

        OPENSSL_cleanse(plaintext.data(), plaintext.size());
    }

    WLOG_ERR("Seed decryption failed for all iteration candidates");
    return std::nullopt;
}

// ============================================================================
// Phase 3D: WalletNotifier interface implementation
// Event-driven wallet updates from blockchain events
// ============================================================================

void WalletManager::onBlockConnected(const Block& block, uint32_t height) {
    if (!hasActiveWallet()) {
        return; // No wallet loaded, nothing to do
    }

    WLOG_INFO("WalletManager: Processing block at height " + std::to_string(height));

    // Update blockchain height for maturity calculations
    setBlockchainHeight(height);

    // Scan all transactions in the block
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        bool is_coinbase = tx.IsCoinbase();

        // Phase 35.1.1: Get real transaction ID (NOT placeholder)
        // Phase M.4.3-B Step 1: Unwrap TxId for string conversion
        std::string txid = tx.GetTxid().AsUint256().GetHex();
        bool existing_history_confirmed = false;

        if (!is_coinbase && current_wallet_id_ != -1) {
            existing_history_confirmed = confirmTransaction(txid, height);
        }

        // Phase 35.1.1: Track if we found any outputs for this transaction
        bool tx_affects_wallet = false;
        double total_received = 0.0;
        std::string receiving_address;

        // Scan all outputs for addresses belonging to this wallet
        for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
            const auto& output = tx.vout[vout];

            // Convert scriptPubKey to hex string
            std::string script_hex;
            for (uint8_t byte : output.scriptPubKey) {
                char buf[3];
                snprintf(buf, sizeof(buf), "%02x", byte);
                script_hex += buf;
            }

            // Check if the scriptPubKey matches any wallet scripts
            if (isScriptMine(script_hex)) {
                // Extract address from scriptPubKey
                std::string address = extractAddressFromScript(output.scriptPubKey);

                // Add UTXO to wallet database
                // Phase M.6.2: Extract value for database boundary (SQLite uses int64_t)
                bool success = addUTXO(
                    txid,
                    static_cast<int>(vout),
                    output.value.GetInt64(),
                    address,
                    script_hex,
                    static_cast<int>(height),
                    is_coinbase
                );

                if (success) {
                    // Phase M.6.2: Extract value for logging/conversion
                        WLOG_INFO("WalletManager: Added UTXO amount: " +
                                          std::to_string(static_cast<double>(output.value.GetUna()) /
                                                         dinero::ConsensusSubsidy::UNA_PER_DIN) + " DIN");

                    // Skip confidential outputs for transaction history — their
                    // value is hidden behind a Pedersen commitment (GetUna()==0).
                    // The shield/unshield RPCs already record the correct amount
                    // via RecordHistory(), so adding a "receive" row here would
                    // either create a duplicate with amount 0 or (on older
                    // schemas without category in the unique constraint)
                    // overwrite the correct shield entry with amount 0.
                    if (output.is_confidential) {
                        WLOG_INFO("WalletManager: Skipping confidential output for tx history "
                                  "(amount recorded by shield RPC): " + txid);
                        continue;
                    }

                    // Phase 35.1.1: Track for transaction history
                    tx_affects_wallet = true;
                    total_received += static_cast<double>(output.value.GetUna()) /
                                      dinero::ConsensusSubsidy::UNA_PER_DIN;
                    if (receiving_address.empty()) {
                        receiving_address = address;
                    }
                }
            }
        }

        // Phase 35.1.1: Record transaction in history if it affects wallet
        if (tx_affects_wallet) {
            if (existing_history_confirmed && !is_coinbase) {
                WLOG_INFO("WalletManager: confirmed existing send/self-spend history for tx " + txid);
            } else {
                std::string category = is_coinbase ? "generate" : "receive";
                std::string label = is_coinbase ? "Mining reward" : "";

                // Use block timestamp for transaction time
                int64_t tx_time = static_cast<int64_t>(block.header.timestamp);

                WLOG_INFO("Phase 35.1.1: Recording transaction to history: " + txid +
                         " (category=" + category + ", amount=" + std::to_string(total_received) +
                         ", address=" + receiving_address + ")");

                // Add transaction to history
                bool tx_added = addTransaction(txid, receiving_address, total_received, category,
                              is_coinbase, label, tx_time, height);

                if (tx_added) {
                    WLOG_INFO("Phase 35.1.1: Successfully added transaction to history");
                } else {
                    WLOG_ERR("Phase 35.1.1: Failed to add transaction to history");
                }
            }
        }

        // Mark wallet UTXOs spent by confirmed transaction inputs.
        // This keeps confirmed balance and change accounting correct.
        int spent_inputs_marked = 0;
        if (!tx.IsCoinbase() && current_wallet_id_ != -1) {
            for (const auto& input : tx.vin) {
                const std::string prev_txid = input.prevout.txid.AsUint256().GetHex();
                const int prev_vout = static_cast<int>(input.prevout.vout);

                sqlite3_stmt* spend_stmt = nullptr;
                const char* spend_sql =
                    "UPDATE utxos SET is_spent = 1 WHERE txid = ? AND vout = ? AND is_spent = 0";

                if (sqlite3_prepare_v2(db_, spend_sql, -1, &spend_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(spend_stmt, 1, prev_txid.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_int(spend_stmt, 2, prev_vout);

                    if (sqlite3_step(spend_stmt) == SQLITE_DONE) {
                        spent_inputs_marked += sqlite3_changes(db_);
                    }
                    sqlite3_finalize(spend_stmt);
                }
            }
        }

        if (spent_inputs_marked > 0) {
            WLOG_INFO("WalletManager: Marked " + std::to_string(spent_inputs_marked) +
                      " wallet inputs as spent for tx " + txid);
        }
    }

    // sync_meta.last_scanned_height is updated by setBlockchainHeight() above
}

void WalletManager::onBlockDisconnected(const Block& block, uint32_t height) {
    if (!hasActiveWallet()) {
        return;  // No wallet loaded, nothing to do
    }

    WLOG_INFO("WalletManager: 🔄 Processing block disconnect at height " + std::to_string(height));

    // Phase 4B: Full reorg handling
    // During a blockchain reorganization, we need to:
    // 1. Remove UTXOs that were created in this block
    // 2. Mark spent UTXOs as unspent again (restore them)

    int utxos_removed = 0;
    int utxos_restored = 0;
    bool tx_history_reverted = false;

    // Step 1: Remove all UTXOs created in this block
    // We can identify them by matching the height column
    if (current_wallet_id_ != -1) {
        sqlite3_stmt* stmt;
        // Note: Per-wallet database - no wallet_id column needed
        const char* sql = "DELETE FROM utxos WHERE height = ?";

        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, static_cast<int>(height));

            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) {
                utxos_removed = sqlite3_changes(db_);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Step 1.5: Remove wallet transactions confirmed in the disconnected block.
    // Prevents "phantom confirmed" transaction history after reorg.
    tx_history_reverted = removeTransactionsAtHeight(height);

    // Step 2: Restore spent UTXOs (mark as unspent)
    // Scan all transactions in the block to find inputs we own
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];

        if (tx.IsCoinbase()) {
            continue;  // Coinbase has no inputs to restore
        }

        // For each input, check if we spent it
        for (const auto& input : tx.vin) {
            // Use canonical prevout txid from input for UTXO restoration.
            std::string prev_txid = input.prevout.txid.AsUint256().GetHex();
            int prev_vout = static_cast<int>(input.prevout.vout);

            // Try to mark this UTXO as unspent (restore it)
            if (current_wallet_id_ != -1) {
                sqlite3_stmt* stmt;
                // Note: Per-wallet database - no wallet_id column needed
                const char* sql = "UPDATE utxos SET is_spent = 0 WHERE txid = ? AND vout = ? AND is_spent = 1";

                int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
                if (rc == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, prev_txid.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_int(stmt, 2, prev_vout);

                    rc = sqlite3_step(stmt);
                    if (rc == SQLITE_DONE && sqlite3_changes(db_) > 0) {
                        utxos_restored++;
                    }
                    sqlite3_finalize(stmt);
                }
            }
        }
    }

    // Step 3: Update wallet blockchain height
    setBlockchainHeight(height - 1);

    // sync_meta.last_scanned_height is updated by setBlockchainHeight(height-1) above

    WLOG_INFO("WalletManager: Block " + std::to_string(height) + " disconnected - " +
                         "removed " + std::to_string(utxos_removed) + " UTXOs, " +
                         "restored " + std::to_string(utxos_restored) + " UTXOs, " +
                         "tx history reverted=" + std::string(tx_history_reverted ? "yes" : "no"));
}

void WalletManager::onMempoolTransaction(const Transaction& tx) {
    if (!hasActiveWallet()) {
        return;
    }

    // Track unconfirmed transactions that involve wallet addresses
    // Phase M.4.3-D: Use TxId directly
    TxId txid = tx.GetTxid();

    WLOG_INFO("WalletManager: 📬 Processing mempool transaction: " + txid.AsUint256().GetHex().substr(0, 16) + "...");

    // Check if transaction involves any wallet outputs
    bool involves_wallet = false;
    for (const auto& output : tx.vout) {
        // Convert scriptPubKey to hex for checking
        std::string script_hex;
        for (uint8_t byte : output.scriptPubKey) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", byte);
            script_hex += buf;
        }

        // Check if this output belongs to wallet
        if (isScriptMine(script_hex)) {
            involves_wallet = true;

            // Extract address from scriptPubKey
            std::string address = extractAddressFromScript(output.scriptPubKey);

            // Phase M.6.2: Extract value for logging
            WLOG_INFO("WalletManager: Detected incoming unconfirmed output: " +
                      std::to_string(static_cast<double>(output.value.GetUna()) /
                                     dinero::ConsensusSubsidy::UNA_PER_DIN) + " DIN to " + address);

            // Note: We don't add to UTXO set until confirmed in a block
            // This prevents spending unconfirmed coins and double-spend issues
            // Balance queries can optionally include pending transactions
        }
    }

    // Check if transaction spends any wallet inputs
    for (const auto& input : tx.vin) {
        // Query if the input spends a wallet UTXO
        // Phase M.4.3-B Step 1: Unwrap TxId for legacy code
        uint256 prevout_txid = input.prevout.txid.AsUint256();
        uint32_t prevout_vout = input.prevout.vout;

        // Check if this input spends one of our UTXOs
        // Note: This would require UTXO lookup, which we handle separately
        // For now, wallet will detect spending when transaction confirms
    }

    if (involves_wallet) {
        WLOG_INFO("WalletManager: Transaction involves wallet, will track when confirmed");
    }

    // Future enhancement: Add to pending_transactions table for:
    // - Showing unconfirmed balance in UI
    // - Detecting double-spend attempts
    // - Faster UI updates before confirmation
    // - RBF (Replace-By-Fee) tracking
}

// ═══════════════════════════════════════════════════════════════════════════
// UTXO INDEX INTEGRATION - Load existing addresses into UTXOIndex
// ═══════════════════════════════════════════════════════════════════════════

void WalletManager::LoadAddressesIntoUTXOIndex() {
    if (!utxo_index_) {
        dinero::g_logger.warning("[WalletManager] UTXOIndex not set - cannot load addresses");
        return;
    }

    if (!db_) {
        dinero::g_logger.error("[WalletManager] No database available for loading addresses");
        return;
    }

    auto load_from_watch_scripts = [&]() -> int {
        const char* sql = "SELECT script_pubkey, path FROM watch_scripts";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            dinero::g_logger.error("[WalletManager] Failed to prepare query for watch_scripts: " +
                                   std::string(sqlite3_errmsg(db_)));
            return 0;
        }

        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            // Get script_pubkey (BLOB)
            const void* script_data = sqlite3_column_blob(stmt, 0);
            const int script_size = sqlite3_column_bytes(stmt, 0);

            // Get path (TEXT)
            const char* path_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (!script_data || script_size <= 0 || !path_cstr) {
                continue;
            }

            std::vector<uint8_t> script_pubkey;
            const auto* bytes = static_cast<const uint8_t*>(script_data);
            script_pubkey.assign(bytes, bytes + script_size);

            utxo_index_->RegisterAddress(script_pubkey, path_cstr);
            count++;
        }

        sqlite3_finalize(stmt);
        return count;
    };

    int count = load_from_watch_scripts();
    if (count > 0) {
        dinero::g_logger.info("[WalletManager] ✅ Loaded " + std::to_string(count) +
                              " addresses into UTXOIndex from watch_scripts");
        return;
    }

    // Recovery path: older/mobile states may have addresses persisted without
    // corresponding watch_scripts rows, which leaves UTXOIndex empty and wallet
    // scans unable to match outputs. Backfill watch_scripts from addresses.
    dinero::g_logger.warning("[WalletManager] watch_scripts empty - attempting backfill from addresses");

    const char* backfill_sql = R"(
        SELECT a.script_pubkey,
               COALESCE(adp.derivation_path,
                        'm/' || CASE WHEN a.type = 'p2tr' THEN '86' ELSE '84' END ||
                        '''/' || ?1 || '''/' || COALESCE(a.account, 0) || '''/' ||
                        COALESCE(a.change, 0) || '/' || COALESCE(a.idx, 0)) AS derivation_path,
               COALESCE(a.change, 0) AS is_change
        FROM addresses a
        LEFT JOIN address_derivation_paths adp ON adp.address = a.address
        WHERE a.script_pubkey IS NOT NULL AND a.script_pubkey <> ''
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, backfill_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        dinero::g_logger.error("[WalletManager] Failed to prepare watch backfill query: " +
                               std::string(sqlite3_errmsg(db_)));
        return;
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(dinero::consensus::DINERO_COIN_TYPE));

    int backfilled = 0;
    int skipped_invalid = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* script_hex_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* path_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const int is_change = sqlite3_column_int(stmt, 2);

        if (!script_hex_cstr || !path_cstr) {
            skipped_invalid++;
            continue;
        }

        std::string script_hex(script_hex_cstr);
        if (script_hex.rfind("0x", 0) == 0 || script_hex.rfind("0X", 0) == 0) {
            script_hex = script_hex.substr(2);
        }

        std::vector<unsigned char> parsed;
        if (!util::unhex(script_hex, parsed) || parsed.empty()) {
            skipped_invalid++;
            continue;
        }

        std::vector<uint8_t> script_bytes(parsed.begin(), parsed.end());
        addWatchScript(script_bytes, path_cstr, is_change != 0);
        backfilled++;
    }
    sqlite3_finalize(stmt);

    if (backfilled > 0) {
        dinero::g_logger.info("[WalletManager] ✅ Backfilled " + std::to_string(backfilled) +
                              " watch scripts from addresses (" + std::to_string(skipped_invalid) +
                              " skipped)");
    } else {
        dinero::g_logger.warning("[WalletManager] watch_scripts backfill found no usable addresses");
    }
}

void WalletManager::addWatchScript(const std::vector<uint8_t>& script_pubkey, const std::string& path, bool is_change) {
    if (!db_) {
        WLOG_ERR("[addWatchScript] No wallet database open");
        return;
    }

    // Insert into watch_scripts table (OR IGNORE prevents duplicates)
    const char* sql = "INSERT OR IGNORE INTO watch_scripts (script_pubkey, path, is_change, last_seen_height, created_at) VALUES (?, ?, ?, 0, ?)";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        WLOG_ERR("[addWatchScript] Failed to prepare SQL: " + std::string(sqlite3_errmsg(db_)));
        return;
    }

    sqlite3_bind_blob(stmt, 1, script_pubkey.data(), script_pubkey.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, is_change ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, std::time(nullptr));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        WLOG_ERR("[addWatchScript] Failed to insert watch script: " + std::string(sqlite3_errmsg(db_)));
        return;
    }

    // Also register with UTXOIndex if available
    if (utxo_index_) {
        utxo_index_->RegisterAddress(script_pubkey, path);
        WLOG_INFO("[addWatchScript] ✅ Registered scriptPubKey with UTXOIndex: " + path);
    }

    WLOG_INFO("[addWatchScript] ✅ Added watch script: " + path);
}

bool WalletManager::storeCovenantDescriptor(
    const CovenantDescriptorRecord& record) {
    if (!db_ ||
        record.descriptor_id.size() != 64 ||
        (record.profile != "ctv" && record.profile != "ccv" &&
         record.profile != "vault") ||
        record.descriptor.empty() ||
        record.script_pubkey.size() != 34 ||
        record.script_pubkey[0] != 0x51 ||
        record.script_pubkey[1] != 0x20) {
        WLOG_ERR("[storeCovenantDescriptor] Invalid record or no active wallet");
        return false;
    }

    const std::string watch_path =
        "m/covenant/1/" + record.descriptor_id;
    sqlite3_stmt* statement = nullptr;
    bool transaction_open = false;
    try {
        exec(db_, "BEGIN IMMEDIATE");
        transaction_open = true;

        const char* insert_sql = R"(
            INSERT OR IGNORE INTO covenant_descriptors
                (descriptor_id, profile, descriptor, script_pubkey, label,
                 parent_descriptor_id)
            VALUES (?, ?, ?, ?, ?, ?)
        )";
        if (sqlite3_prepare_v2(
                db_, insert_sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(
            statement, 1, record.descriptor_id.c_str(), -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_text(
            statement, 2, record.profile.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            statement, 3, record.descriptor.c_str(), -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_blob(
            statement, 4,
            record.script_pubkey.data(),
            static_cast<int>(record.script_pubkey.size()),
            SQLITE_TRANSIENT);
        sqlite3_bind_text(
            statement, 5, record.label.c_str(), -1, SQLITE_TRANSIENT);
        if (record.parent_descriptor_id.empty()) {
            sqlite3_bind_null(statement, 6);
        } else {
            sqlite3_bind_text(
                statement, 6,
                record.parent_descriptor_id.c_str(), -1,
                SQLITE_TRANSIENT);
        }
        if (sqlite3_step(statement) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(statement);
        statement = nullptr;

        // INSERT OR IGNORE is idempotent, but an impossible descriptor-id or
        // script collision must fail closed rather than silently aliasing two
        // recovery records.
        const char* verify_sql = R"(
            SELECT profile, descriptor, script_pubkey
            FROM covenant_descriptors
            WHERE descriptor_id = ?
        )";
        if (sqlite3_prepare_v2(
                db_, verify_sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(
            statement, 1, record.descriptor_id.c_str(), -1,
            SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            throw std::runtime_error("covenant descriptor insert disappeared");
        }
        const char* stored_profile =
            reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0));
        const char* stored_descriptor =
            reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 1));
        const auto* stored_script =
            static_cast<const uint8_t*>(
                sqlite3_column_blob(statement, 2));
        const int stored_script_size =
            sqlite3_column_bytes(statement, 2);
        const bool exact_match =
            stored_profile != nullptr &&
            stored_descriptor != nullptr &&
            record.profile == stored_profile &&
            record.descriptor == stored_descriptor &&
            stored_script != nullptr &&
            stored_script_size ==
                static_cast<int>(record.script_pubkey.size()) &&
            std::equal(
                record.script_pubkey.begin(),
                record.script_pubkey.end(),
                stored_script);
        sqlite3_finalize(statement);
        statement = nullptr;
        if (!exact_match) {
            throw std::runtime_error(
                "covenant descriptor identifier or script collision");
        }

        const char* watch_sql = R"(
            INSERT OR IGNORE INTO watch_scripts
                (script_pubkey, path, is_change, last_seen_height, created_at)
            VALUES (?, ?, 0, 0, strftime('%s','now'))
        )";
        if (sqlite3_prepare_v2(
                db_, watch_sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_blob(
            statement, 1,
            record.script_pubkey.data(),
            static_cast<int>(record.script_pubkey.size()),
            SQLITE_TRANSIENT);
        sqlite3_bind_text(
            statement, 2, watch_path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(statement);
        statement = nullptr;

        // The script is the primary key. INSERT OR IGNORE may therefore have
        // preserved a pre-existing wallet/watch registration with a different
        // ownership path. Committing the descriptor in that case would make
        // SQLite recovery and the in-memory UTXO index disagree about who
        // owns the script.
        const char* verify_watch_sql = R"(
            SELECT path, is_change
            FROM watch_scripts
            WHERE script_pubkey = ?
        )";
        if (sqlite3_prepare_v2(
                db_, verify_watch_sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_blob(
            statement, 1,
            record.script_pubkey.data(),
            static_cast<int>(record.script_pubkey.size()),
            SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            throw std::runtime_error("covenant watch-script insert disappeared");
        }
        const char* stored_path =
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const bool watch_matches =
            stored_path != nullptr &&
            watch_path == stored_path &&
            sqlite3_column_int(statement, 1) == 0;
        sqlite3_finalize(statement);
        statement = nullptr;
        if (!watch_matches) {
            throw std::runtime_error(
                "covenant script collides with an existing watch path");
        }

        exec(db_, "COMMIT");
        transaction_open = false;
    } catch (const std::exception& error) {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
        if (transaction_open) {
            try {
                exec(db_, "ROLLBACK");
            } catch (...) {
            }
        }
        WLOG_ERR(
            "[storeCovenantDescriptor] Failed: " +
            std::string(error.what()));
        return false;
    }

    if (utxo_index_) {
        utxo_index_->RegisterAddress(record.script_pubkey, watch_path);
    }
    WLOG_INFO(
        "[storeCovenantDescriptor] Stored " + record.profile +
        " descriptor " + record.descriptor_id);
    return true;
}

std::optional<CovenantDescriptorRecord>
WalletManager::getCovenantDescriptor(
    const std::string& descriptor_id) const {
    if (!db_ || descriptor_id.empty()) {
        return std::nullopt;
    }
    const char* sql = R"(
        SELECT descriptor_id, profile, descriptor, script_pubkey, label,
               COALESCE(parent_descriptor_id, ''), created_at
        FROM covenant_descriptors
        WHERE descriptor_id = ?
    )";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(
        statement, 1, descriptor_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    CovenantDescriptorRecord result;
    const auto text = [&](int column) -> std::string {
        const char* value =
            reinterpret_cast<const char*>(
                sqlite3_column_text(statement, column));
        return value ? value : "";
    };
    result.descriptor_id = text(0);
    result.profile = text(1);
    result.descriptor = text(2);
    const auto* script =
        static_cast<const uint8_t*>(
            sqlite3_column_blob(statement, 3));
    const int script_size = sqlite3_column_bytes(statement, 3);
    if (script != nullptr && script_size > 0) {
        result.script_pubkey.assign(script, script + script_size);
    }
    result.label = text(4);
    result.parent_descriptor_id = text(5);
    result.created_at = sqlite3_column_int64(statement, 6);
    sqlite3_finalize(statement);
    return result;
}

std::vector<CovenantDescriptorRecord>
WalletManager::listCovenantDescriptors() const {
    std::vector<CovenantDescriptorRecord> result;
    if (!db_) {
        return result;
    }
    const char* sql = R"(
        SELECT descriptor_id
        FROM covenant_descriptors
        ORDER BY created_at, descriptor_id
    )";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return result;
    }
    std::vector<std::string> ids;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* id =
            reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0));
        if (id != nullptr) {
            ids.emplace_back(id);
        }
    }
    sqlite3_finalize(statement);

    result.reserve(ids.size());
    for (const auto& id : ids) {
        auto record = getCovenantDescriptor(id);
        if (record.has_value()) {
            result.push_back(std::move(*record));
        }
    }
    return result;
}

void WalletManager::addAddress(int account, int change, int idx, const std::string& address, const std::string& type) {
    if (!db_) {
        WLOG_ERR("[addAddress] No wallet database open");
        return;
    }

    // Insert into addresses table (OR IGNORE prevents duplicates)
    const char* sql = "INSERT OR IGNORE INTO addresses (account, change, idx, address, type, created_at) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        WLOG_ERR("[addAddress] Failed to prepare SQL: " + std::string(sqlite3_errmsg(db_)));
        return;
    }

    sqlite3_bind_int(stmt, 1, account);
    sqlite3_bind_int(stmt, 2, change);
    sqlite3_bind_int(stmt, 3, idx);
    sqlite3_bind_text(stmt, 4, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, std::time(nullptr));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        WLOG_ERR("[addAddress] Failed to insert address: " + std::string(sqlite3_errmsg(db_)));
        return;
    }

    WLOG_INFO("[addAddress] ✅ Added address to wallet: " + address);
}

// ═══════════════════════════════════════════════════════════════
// Taproot Descriptor Import (BIP341 Compliant)
// ═══════════════════════════════════════════════════════════════
// These methods support importing single Taproot keys via tr() descriptor.
// The internal key is stored for signing; the tweaked output key is used
// for address derivation and UTXO matching.
// ═══════════════════════════════════════════════════════════════

void WalletManager::registerTaprootAddress(const std::vector<uint8_t>& script_pubkey,
                                           const std::string& derivation_path,
                                           const std::array<uint8_t, 32>& internal_pubkey,
                                           const std::array<uint8_t, 32>& output_pubkey) {
    if (!db_) {
        WLOG_ERR("[registerTaprootAddress] No wallet database open");
        return;
    }

    // Register with UTXOIndex for UTXO scanning
    if (utxo_index_) {
        utxo_index_->RegisterAddress(script_pubkey, derivation_path);
        WLOG_INFO("[registerTaprootAddress] ✅ Registered scriptPubKey with UTXOIndex");
    } else {
        WLOG_ERR("[registerTaprootAddress] UTXOIndex not available - address will not be scanned!");
    }

    // Also add to watch_scripts table for persistence
    const char* sql = "INSERT OR IGNORE INTO watch_scripts (script_pubkey, path, is_change, last_seen_height, created_at) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, script_pubkey.data(), script_pubkey.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, derivation_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, 0);  // not change
        sqlite3_bind_int(stmt, 4, 0);  // last_seen_height
        sqlite3_bind_int64(stmt, 5, std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        WLOG_INFO("[registerTaprootAddress] ✅ Persisted to watch_scripts table");
    }

    // Store internal/output pubkey mapping for future reference
    const char* mapping_sql = "INSERT OR REPLACE INTO taproot_key_mapping (output_pubkey, internal_pubkey, derivation_path, created_at) VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, mapping_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, output_pubkey.data(), 32, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, internal_pubkey.data(), 32, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, derivation_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        WLOG_INFO("[registerTaprootAddress] ✅ Stored internal/output pubkey mapping");
    }
}

void WalletManager::registerP2MRAddress(const std::vector<uint8_t>& script_pubkey,
                                        const std::string& derivation_path) {
    if (!db_) {
        WLOG_ERR("[registerP2MRAddress] No wallet database open");
        return;
    }

    // Live registration: UTXOIndex scans incoming tx outputs against
    // watched_scripts_ via IsOurScript. Without this the P2MR output
    // this address receives funds at won't get indexed as wallet-owned.
    if (utxo_index_) {
        utxo_index_->RegisterAddress(script_pubkey, derivation_path);
        WLOG_INFO("[registerP2MRAddress] ✅ Registered P2MR scriptPubKey with UTXOIndex (path=" +
                  derivation_path + ")");
    } else {
        WLOG_ERR("[registerP2MRAddress] UTXOIndex not available - address will not be scanned");
    }

    // Persistence: watch_scripts is replayed on wallet unlock by
    // LoadAddressesIntoUTXOIndex. Taproot uses the same table; P2MR
    // scriptPubKeys (34 bytes, leading 0x53 0x20) sit alongside
    // Taproot (leading 0x51 0x20) with no schema change required.
    const char* sql = "INSERT OR IGNORE INTO watch_scripts "
                      "(script_pubkey, path, is_change, last_seen_height, created_at) "
                      "VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, script_pubkey.data(),
                          static_cast<int>(script_pubkey.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, derivation_path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, 0);
        sqlite3_bind_int(stmt, 4, 0);
        sqlite3_bind_int64(stmt, 5, std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        WLOG_INFO("[registerP2MRAddress] ✅ Persisted to watch_scripts");
    } else {
        WLOG_ERR("[registerP2MRAddress] watch_scripts insert failed to prepare: " +
                 std::string(sqlite3_errmsg(db_)));
    }
}

void WalletManager::storeTaprootKey(const std::string& address,
                                    const std::array<uint8_t, 32>& internal_privkey,
                                    const std::array<uint8_t, 32>& internal_pubkey,
                                    const std::array<uint8_t, 32>& output_pubkey,
                                    const std::string& label) {
    if (!db_) {
        WLOG_ERR("[storeTaprootKey] No wallet database open");
        return;
    }

    // Create taproot_keys table if it doesn't exist.
    // is_privkey_encrypted: 1 = AES-256-GCM encrypted under wallet key, 0 = raw (unencrypted wallet)
    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS taproot_keys (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            address TEXT UNIQUE NOT NULL,
            internal_privkey BLOB NOT NULL,
            internal_pubkey BLOB NOT NULL,
            output_pubkey BLOB NOT NULL,
            label TEXT,
            created_at INTEGER NOT NULL,
            is_privkey_encrypted INTEGER NOT NULL DEFAULT 0
        )
    )";
    sqlite3_exec(db_, create_sql, nullptr, nullptr, nullptr);
    // Migration: add column for wallets created before this change
    sqlite3_exec(db_, "ALTER TABLE taproot_keys ADD COLUMN is_privkey_encrypted INTEGER NOT NULL DEFAULT 0",
                 nullptr, nullptr, nullptr);

    // Create taproot_key_mapping table if needed
    const char* mapping_create_sql = R"(
        CREATE TABLE IF NOT EXISTS taproot_key_mapping (
            output_pubkey BLOB PRIMARY KEY,
            internal_pubkey BLOB NOT NULL,
            derivation_path TEXT,
            created_at INTEGER NOT NULL
        )
    )";
    sqlite3_exec(db_, mapping_create_sql, nullptr, nullptr, nullptr);

    // Encrypt the imported private key under the wallet encryption key if available.
    // Unencrypted wallets store it raw (matching the unencrypted seed semantics).
    std::vector<uint8_t> privkey_blob(internal_privkey.begin(), internal_privkey.end());
    int is_privkey_encrypted = 0;
    if (!encryption_key_.empty()) {
        try {
            std::string raw(internal_privkey.begin(), internal_privkey.end());
            std::string enc = encryptData(raw, encryption_key_);
            privkey_blob.assign(enc.begin(), enc.end());
            is_privkey_encrypted = 1;
        } catch (const std::exception& e) {
            WLOG_ERR("[storeTaprootKey] Encryption failed, storing plaintext: " + std::string(e.what()));
        }
    }

    const char* sql = "INSERT OR REPLACE INTO taproot_keys "
                      "(address, internal_privkey, internal_pubkey, output_pubkey, label, created_at, is_privkey_encrypted) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, privkey_blob.data(), static_cast<int>(privkey_blob.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, internal_pubkey.data(), 32, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 4, output_pubkey.data(), 32, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, label.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 6, std::time(nullptr));
        sqlite3_bind_int(stmt, 7, is_privkey_encrypted);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            WLOG_INFO("[storeTaprootKey] ✅ Stored Taproot key for address: " + address +
                      (is_privkey_encrypted ? " (encrypted)" : " (plaintext — wallet not encrypted)"));
        } else {
            WLOG_ERR("[storeTaprootKey] Failed to store key: " + std::string(sqlite3_errmsg(db_)));
        }
    } else {
        WLOG_ERR("[storeTaprootKey] Failed to prepare SQL: " + std::string(sqlite3_errmsg(db_)));
    }

    // Also add to main addresses table for address book
    const char* addr_sql = "INSERT OR IGNORE INTO addresses (account, change, idx, address, type, created_at) VALUES (?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, addr_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, -1);  // -1 indicates imported (not HD derived)
        sqlite3_bind_int(stmt, 2, 0);
        sqlite3_bind_int(stmt, 3, 0);
        sqlite3_bind_text(stmt, 4, address.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, "taproot_imported", -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 6, std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Set label
    setAddressLabel(address, label);
}

// ═══════════════════════════════════════════════════════════════
// Phase: Wallet Security - Encryption metadata storage
// ═══════════════════════════════════════════════════════════════

bool WalletManager::storeEncryptedWallet(
    const std::string& wallet_name,
    const std::vector<uint8_t>& encrypted_seed_with_tag,
    const std::vector<uint8_t>& salt,
    const std::vector<uint8_t>& nonce,
    int argon2_iterations,
    int argon2_memory_kb,
    int argon2_parallelism,
    uint32_t master_fingerprint
) {
    try {
        // Per-wallet DB: No wallet_id needed (always id=1)
        if (!db_) {
            WLOG_ERR("No wallet database open");
            return false;
        }

        // Validate inputs
        if (encrypted_seed_with_tag.empty()) {
            WLOG_ERR("Encrypted seed is empty");
            return false;
        }
        if (salt.size() != 16) {
            WLOG_ERR("Invalid salt size (expected 16 bytes, got " + std::to_string(salt.size()) + ")");
            return false;
        }
        if (nonce.size() != 12) {
            WLOG_ERR("Invalid nonce size (expected 12 bytes, got " + std::to_string(nonce.size()) + ")");
            return false;
        }

        // Convert master_fingerprint to 4-byte BLOB
        std::vector<uint8_t> fingerprint_blob(4);
        fingerprint_blob[0] = (master_fingerprint >> 24) & 0xFF;
        fingerprint_blob[1] = (master_fingerprint >> 16) & 0xFF;
        fingerprint_blob[2] = (master_fingerprint >> 8) & 0xFF;
        fingerprint_blob[3] = master_fingerprint & 0xFF;

        // Store encrypted seed in hd_seeds table (id=1 for per-wallet DB)
        sqlite3_stmt* stmt = nullptr;
        const char* sql_seed = R"(
            INSERT OR REPLACE INTO hd_seeds (id, encrypted_seed, salt, coin_type, encryption_version, created_at)
            VALUES (1, ?, ?, ?, 2, strftime('%s','now'))
        )";

        if (sqlite3_prepare_v2(db_, sql_seed, -1, &stmt, nullptr) != SQLITE_OK) {
            WLOG_ERR("Failed to prepare statement for storing encrypted seed");
            return false;
        }

        sqlite3_bind_blob(stmt, 1, encrypted_seed_with_tag.data(), encrypted_seed_with_tag.size(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, salt.data(), salt.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, static_cast<int>(dinero::consensus::DINERO_COIN_TYPE));

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            WLOG_ERR("Failed to store encrypted seed: " + std::string(sqlite3_errmsg(db_)));
            return false;
        }

        // Store encryption metadata (id=1 for per-wallet DB)
        const char* sql_meta = R"(
            INSERT OR REPLACE INTO encryption_metadata (
                id, encrypted, kdf, kdf_iterations, kdf_memory_kb, kdf_parallelism,
                cipher, salt, nonce, created_at, updated_at
            )
            VALUES (1, 1, 'argon2id', ?, ?, ?, 'AES-256-GCM', ?, ?, strftime('%s','now'), strftime('%s','now'))
        )";

        if (sqlite3_prepare_v2(db_, sql_meta, -1, &stmt, nullptr) != SQLITE_OK) {
            WLOG_ERR("Failed to prepare statement for storing encryption metadata");
            return false;
        }

        sqlite3_bind_int(stmt, 1, argon2_iterations);
        sqlite3_bind_int(stmt, 2, argon2_memory_kb);
        sqlite3_bind_int(stmt, 3, argon2_parallelism);
        sqlite3_bind_blob(stmt, 4, salt.data(), salt.size(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 5, nonce.data(), nonce.size(), SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            WLOG_ERR("Failed to store encryption metadata: " + std::string(sqlite3_errmsg(db_)));
            return false;
        }

        // Update wallet_meta with encryption flag and fingerprint
        const char* sql_wallet_meta = "UPDATE wallet_meta SET encrypted = 1, fingerprint = ? WHERE id = 1";
        if (sqlite3_prepare_v2(db_, sql_wallet_meta, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_blob(stmt, 1, fingerprint_blob.data(), fingerprint_blob.size(), SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Update registry with encryption flag and fingerprint
        if (registry_db_) {
            std::filesystem::path walletPath = dataDir_ / "wallets" / ("wallet_" + wallet_name + ".db");
            registerWalletInRegistry(wallet_name, walletPath.string(), "mainnet", true, fingerprint_blob);
        }

        WLOG_INFO("✅ Stored encrypted wallet metadata for: " + wallet_name);
        return true;

    } catch (const std::exception& e) {
        WLOG_ERR("Exception while storing encrypted wallet: " + std::string(e.what()));
        return false;
    }
}

bool WalletManager::storeUnencryptedWallet(
    const std::string& wallet_name,
    const std::vector<uint8_t>& seed,
    uint32_t master_fingerprint,
    bool seed_already_stored
) {
    try {
        // Per-wallet DB: No wallet_id needed (always id=1)
        if (!db_) {
            WLOG_ERR("No wallet database open");
            return false;
        }

        // Validate inputs
        if (seed.empty()) {
            WLOG_ERR("Seed is empty");
            return false;
        }

        // Convert master_fingerprint to 4-byte BLOB
        std::vector<uint8_t> fingerprint_blob(4);
        fingerprint_blob[0] = (master_fingerprint >> 24) & 0xFF;
        fingerprint_blob[1] = (master_fingerprint >> 16) & 0xFF;
        fingerprint_blob[2] = (master_fingerprint >> 8) & 0xFF;
        fingerprint_blob[3] = master_fingerprint & 0xFF;

        if (seed_already_stored) {
            // createFromBip39() persisted this exact seed and its recovery
            // binding before registry publication. Do not rewrite it here:
            // storeMasterSeed(reset=true) would briefly clear that binding.
            if (!ConstantTimeEqual(seed, master_seed_)) {
                WLOG_ERR("Pre-stored seed does not match active wallet identity");
                return false;
            }
        } else {
            // Existing-wallet replacement/restoration still needs to persist
            // the supplied seed and reset its derivation state.
            if (!storeMasterSeed(seed, "")) {
                WLOG_ERR("Failed to store unencrypted seed");
                return false;
            }
        }

        // Store encryption metadata (mark as unencrypted, id=1 for per-wallet DB)
        sqlite3_stmt* stmt = nullptr;
        const char* sql_meta = R"(
            INSERT OR REPLACE INTO encryption_metadata (
                id, encrypted, kdf, cipher, created_at, updated_at
            )
            VALUES (1, 0, 'none', 'none', strftime('%s','now'), strftime('%s','now'))
        )";

        if (sqlite3_prepare_v2(db_, sql_meta, -1, &stmt, nullptr) != SQLITE_OK) {
            WLOG_ERR("Failed to prepare statement for storing unencrypted metadata");
            return false;
        }

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            WLOG_ERR("Failed to store unencrypted metadata: " + std::string(sqlite3_errmsg(db_)));
            return false;
        }

        // Update wallet_meta with fingerprint
        const char* sql_wallet_meta = "UPDATE wallet_meta SET encrypted = 0, fingerprint = ? WHERE id = 1";
        if (sqlite3_prepare_v2(db_, sql_wallet_meta, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_blob(stmt, 1, fingerprint_blob.data(), fingerprint_blob.size(), SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Update registry with fingerprint
        if (registry_db_) {
            std::filesystem::path walletPath = dataDir_ / "wallets" / ("wallet_" + wallet_name + ".db");
            registerWalletInRegistry(wallet_name, walletPath.string(), "mainnet", false, fingerprint_blob);
        }

        WLOG_INFO("✅ Stored unencrypted wallet metadata for: " + wallet_name);
        return true;

    } catch (const std::exception& e) {
        WLOG_ERR("Exception while storing unencrypted wallet: " + std::string(e.what()));
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Week 1 Day 5: WalletKeyStore Interface Implementation
// Enables IsMine script ownership queries for descriptor wallet
// ═══════════════════════════════════════════════════════════════════════

bool WalletManager::HaveKey(const wallet::KeyID& key_id) const {
    if (!db_) {
        return false;
    }

    const char* sql = "SELECT COUNT(*) FROM addresses WHERE key_id = ?";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, key_id.data(), key_id.size(), SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            return count > 0;
        }
        sqlite3_finalize(stmt);
    }

    return false;
}

std::optional<wallet::WalletKey> WalletManager::GetKey(const wallet::KeyID& key_id) const {
    if (!db_) {
        return std::nullopt;
    }

    const char* sql = "SELECT key_id, internal_key_id, output_key_id, address, script_pubkey FROM addresses WHERE key_id = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, key_id.data(), key_id.size(), SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            wallet::WalletKey key;
            key.id = key_id;
            key.spendable = !master_seed_.empty();  // Spendable if we have master seed

            // Get scriptPubKey for derivation path lookup (Bitcoin Core semantics)
            // ⚠️ OWNERSHIP LOGIC - Uses scriptPubKey (consensus data), NOT address
            const unsigned char* spk_ptr = sqlite3_column_text(stmt, 4);
            if (spk_ptr) {
                std::string script_pubkey(reinterpret_cast<const char*>(spk_ptr));

                // Get derivation path by scriptPubKey
                auto path_opt = getDerivationPath(script_pubkey);
                if (path_opt.has_value()) {
                    // Parse KeyOriginInfo from path string like "m/86'/1448'/0'/0/12"
                    auto origin_opt = wallet::KeyOriginInfo::parsePathString(path_opt.value());
                    if (origin_opt.has_value()) {
                        key.origin = origin_opt.value();
                        // BIP32 fingerprint: first 4 bytes of HASH160(master_pubkey)
                        if (!master_seed_.empty()) {
                            try {
                                auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);
                                auto h160 = master.getHash160();
                                key.origin.fingerprint =
                                    (static_cast<uint32_t>(h160[0]) << 24) |
                                    (static_cast<uint32_t>(h160[1]) << 16) |
                                    (static_cast<uint32_t>(h160[2]) << 8)  |
                                     static_cast<uint32_t>(h160[3]);
                            } catch (...) {
                                key.origin.fingerprint = 0;
                            }
                        } else {
                            key.origin.fingerprint = 0;
                        }
                    }
                }
            }

            // Get internal_key_id if present (Taproot)
            if (sqlite3_column_type(stmt, 1) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 1);
                int blob_size = sqlite3_column_bytes(stmt, 1);
                if (blob_size == 20) {
                    wallet::KeyID internal_kid;
                    std::memcpy(internal_kid.data(), blob, 20);
                    key.internal_key_id = internal_kid;
                }
            }

            // Get output_key_id if present (Taproot)
            if (sqlite3_column_type(stmt, 2) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 2);
                int blob_size = sqlite3_column_bytes(stmt, 2);
                if (blob_size == 20) {
                    wallet::KeyID output_kid;
                    std::memcpy(output_kid.data(), blob, 20);
                    key.output_key_id = output_kid;
                }
            }

            sqlite3_finalize(stmt);
            return key;
        }
        sqlite3_finalize(stmt);
    }

    return std::nullopt;
}

std::optional<wallet::WalletKey> WalletManager::GetKeyByOutputKeyID(const wallet::KeyID& output_key_id) const {
    if (!db_) {
        return std::nullopt;
    }

    // CRITICAL: For Taproot, look up by output_key_id column, not key_id
    const char* sql = "SELECT key_id, internal_key_id, output_key_id, address, script_pubkey FROM addresses WHERE output_key_id = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, output_key_id.data(), output_key_id.size(), SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            wallet::WalletKey key;
            key.spendable = !master_seed_.empty();

            // Get key_id (primary identifier)
            if (sqlite3_column_type(stmt, 0) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 0);
                int blob_size = sqlite3_column_bytes(stmt, 0);
                if (blob_size == 20) {
                    std::memcpy(key.id.data(), blob, 20);
                }
            }

            // Get internal_key_id
            if (sqlite3_column_type(stmt, 1) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 1);
                int blob_size = sqlite3_column_bytes(stmt, 1);
                if (blob_size == 20) {
                    wallet::KeyID internal_kid;
                    std::memcpy(internal_kid.data(), blob, 20);
                    key.internal_key_id = internal_kid;
                }
            }

            // Get output_key_id
            key.output_key_id = output_key_id;

            // Get scriptPubKey for derivation path lookup (Bitcoin Core semantics)
            // ⚠️ OWNERSHIP LOGIC - Uses scriptPubKey (consensus data), NOT address
            const unsigned char* spk_ptr = sqlite3_column_text(stmt, 4);
            if (spk_ptr) {
                std::string script_pubkey(reinterpret_cast<const char*>(spk_ptr));

                // Get derivation path for KeyOriginInfo
                auto path_opt = getDerivationPath(script_pubkey);
                if (path_opt.has_value()) {
                    // Parse KeyOriginInfo from path string like "m/86'/1448'/0'/0/12"
                    auto origin_opt = wallet::KeyOriginInfo::parsePathString(path_opt.value());
                    if (origin_opt.has_value()) {
                        key.origin = origin_opt.value();
                        // BIP32 fingerprint: first 4 bytes of HASH160(master_pubkey)
                        if (!master_seed_.empty()) {
                            try {
                                auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);
                                auto h160 = master.getHash160();
                                key.origin.fingerprint =
                                    (static_cast<uint32_t>(h160[0]) << 24) |
                                    (static_cast<uint32_t>(h160[1]) << 16) |
                                    (static_cast<uint32_t>(h160[2]) << 8)  |
                                     static_cast<uint32_t>(h160[3]);
                            } catch (...) {
                                key.origin.fingerprint = 0;
                            }
                        } else {
                            key.origin.fingerprint = 0;
                        }
                    }
                }
            }

            sqlite3_finalize(stmt);
            return key;
        }
        sqlite3_finalize(stmt);
    }

    return std::nullopt;
}

std::vector<wallet::WalletKey> WalletManager::GetAllKeys() const {
    std::vector<wallet::WalletKey> keys;

    if (!db_) {
        return keys;
    }

    const char* sql = "SELECT key_id, internal_key_id, output_key_id, address FROM addresses WHERE key_id IS NOT NULL";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            wallet::WalletKey key;
            key.spendable = !master_seed_.empty();

            // Get key_id
            if (sqlite3_column_type(stmt, 0) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 0);
                int blob_size = sqlite3_column_bytes(stmt, 0);
                if (blob_size == 20) {
                    std::memcpy(key.id.data(), blob, 20);
                }
            }

            // Get internal_key_id if present
            if (sqlite3_column_type(stmt, 1) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 1);
                int blob_size = sqlite3_column_bytes(stmt, 1);
                if (blob_size == 20) {
                    wallet::KeyID internal_kid;
                    std::memcpy(internal_kid.data(), blob, 20);
                    key.internal_key_id = internal_kid;
                }
            }

            // Get output_key_id if present
            if (sqlite3_column_type(stmt, 2) == SQLITE_BLOB) {
                const void* blob = sqlite3_column_blob(stmt, 2);
                int blob_size = sqlite3_column_bytes(stmt, 2);
                if (blob_size == 20) {
                    wallet::KeyID output_kid;
                    std::memcpy(output_kid.data(), blob, 20);
                    key.output_key_id = output_kid;
                }
            }

            keys.push_back(key);
        }
        sqlite3_finalize(stmt);
    }

    return keys;
}

bool WalletManager::AddKey(const wallet::WalletKey& key) {
    // Not implemented - keys are added via getNewAddress()
    // This method is here for interface compliance
    return false;
}

bool WalletManager::HaveMasterSeed() const {
    return !master_seed_.empty();
}

std::optional<std::vector<uint8_t>> WalletManager::GetMasterSeed() const {
    if (master_seed_.empty()) {
        return std::nullopt;
    }
    return master_seed_;
}

std::optional<std::vector<uint8_t>> WalletManager::DerivePrivateKey(
    const wallet::KeyOriginInfo& origin) const {

    if (master_seed_.empty()) {
        // Defensive recovery: try reloading seed from DB if wallet is currently unlocked.
        auto* self = const_cast<WalletManager*>(this);
        if (!self->wallet_locked_) {
            auto seed_opt = self->loadMasterSeed("");
            if (seed_opt.has_value()) {
                self->master_seed_ = seed_opt.value();
                WLOG_INFO("[DerivePrivateKey] Recovered master seed in-memory from database");
            }
        }

        if (master_seed_.empty()) {
            WLOG_ERR("[DerivePrivateKey] No master seed available");
            return std::nullopt;
        }
    }

    try {
        // Use BIP32Deriver (same engine as hd_wallet.cpp) for consistent derivation
        dinero::BIP32Deriver deriver(master_seed_.data(), master_seed_.size());

        // Path components have hardened bit (0x80000000) pre-set
        for (uint32_t component : origin.path) {
            if (component & 0x80000000) {
                deriver.deriveHardened(component & ~0x80000000);
            } else {
                deriver.deriveNormal(component);
            }
        }

        auto privkey = deriver.getPrivateKey();
        return std::vector<uint8_t>(privkey.begin(), privkey.end());

    } catch (const std::exception& e) {
        WLOG_ERR("[DerivePrivateKey] Failed to derive key: " + std::string(e.what()));
        return std::nullopt;
    }
}

} // namespace dinero
