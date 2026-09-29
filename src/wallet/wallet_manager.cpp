#include "wallet/selected_history.h"
#include "wallet/unsigned_tx_builder.h"
#include <climits>
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
#include "wallet/p2mr_address.h"
#include "wallet/secure_keypair.h"
#include "consensus/pq/scheme_registry.h"
#include "crypto/sha256.h"
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
#include <limits>
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

struct ScopedShieldedAccountKeys {
    wallet::shielded::ShieldedAccountKeys* keys = nullptr;

    explicit ScopedShieldedAccountKeys(
        wallet::shielded::ShieldedAccountKeys& input) : keys(&input) {}
    ScopedShieldedAccountKeys(const ScopedShieldedAccountKeys&) = delete;
    ScopedShieldedAccountKeys& operator=(const ScopedShieldedAccountKeys&) = delete;
    ~ScopedShieldedAccountKeys() {
        if (keys != nullptr) OPENSSL_cleanse(keys, sizeof(*keys));
    }
};

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

constexpr char kInitialOwnerSetting[] = "wallet_initial_owner_v1";
std::string InitialOwnerKey(const std::vector<uint8_t>& seed) {
    if (seed.size() != 64) throw std::runtime_error("Initialization seed unavailable");
    const std::string domain = "Dinero WalletManager initial PQ owner v1";
    std::vector<uint8_t> material(domain.begin(), domain.end());
    material.insert(material.end(), seed.begin(), seed.end());
    std::array<uint8_t, 32> key{};
    ::sha256(material.data(), material.size(), key.data());
    secureClearBytes(material);
    std::string result(reinterpret_cast<const char*>(key.data()), key.size());
    OPENSSL_cleanse(key.data(), key.size());
    return result;
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

WalletManager::GeneratedBip39Identity::~GeneratedBip39Identity() {
    secureClearString(mnemonic_);
    secureClearBytes(seed_);
}

WalletManager::GeneratedBip39Identity WalletManager::GenerateBip39Identity(
    int word_count, const std::string& passphrase) {
    bip39::WordCount words;
    switch (word_count) {
    case 12: words = bip39::WordCount::Words12; break;
    case 15: words = bip39::WordCount::Words15; break;
    case 18: words = bip39::WordCount::Words18; break;
    case 21: words = bip39::WordCount::Words21; break;
    case 24: words = bip39::WordCount::Words24; break;
    default: throw std::invalid_argument("Invalid BIP39 word count");
    }
    GeneratedBip39Identity identity;
    identity.mnemonic_ = bip39::Generate(words);
    if (identity.mnemonic_.empty() ||
        !bip39::MnemonicToSeed(identity.mnemonic_, passphrase, identity.seed_) ||
        identity.seed_.size() != 64)
        throw std::runtime_error("Failed to generate BIP39 identity");
    return identity;
}

void WalletManager::createFromGeneratedBip39(const std::string& name,
    GeneratedBip39Identity&& identity, const std::string& passphrase) {
    // Consume the generated identity. A moved-from or reused ticket cannot
    // authorize a second initialization, and recovery has no ticket constructor.
    GeneratedBip39Identity owned(std::move(identity));
    secureClearString(identity.mnemonic_);
    secureClearBytes(identity.seed_);
    std::vector<uint8_t> check;
    const bool matches = bip39::MnemonicToSeed(owned.mnemonic_, passphrase, check) &&
                         ConstantTimeEqual(check, owned.seed_) && check.size() == 64;
    secureClearBytes(check);
    if (!matches) throw std::runtime_error("Generated BIP39 identity unavailable or mismatched");
    createWithInitialSeed(name, owned.seed_, &owned.mnemonic_, passphrase, InitialSeedKind::Generated);
}

void WalletManager::create(const std::string& name) {
    std::vector<uint8_t> seed(64);
    if (!CF_GenerateRandomBytes(seed.data(), seed.size())) {
        throw std::runtime_error("Failed to generate initial HD wallet seed");
    }
    try {
        createWithInitialSeed(name, seed, nullptr, "", InitialSeedKind::Generated);
    } catch (...) {
        secureClearBytes(seed);
        throw;
    }
    secureClearBytes(seed);
}

void WalletManager::createFromBip39(const std::string& name,
                                    const std::string& mnemonic,
                                    const std::string& bip39_passphrase,
                                    bool skip_checksum) {
    if (!skip_checksum && !bip39::ValidateMnemonic(mnemonic)) {
        throw std::invalid_argument("Invalid BIP39 mnemonic");
    }
    std::vector<uint8_t> seed;
    if (!bip39::MnemonicToSeed(mnemonic, bip39_passphrase, seed, skip_checksum) || seed.size() != 64) {
        secureClearBytes(seed);
        throw std::runtime_error("Failed to derive BIP39 wallet seed");
    }
    try {
        createWithInitialSeed(name, seed, skip_checksum ? nullptr : &mnemonic, bip39_passphrase, InitialSeedKind::Recovered);
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
    const std::string& bip39_passphrase, InitialSeedKind kind) {
    std::lock_guard<std::recursive_mutex> database_lock(database_lifecycle_mutex_);
    if (database_leases_ != 0) throw std::logic_error("Cannot create a wallet during database delivery");
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
            AdvanceDatabaseSession();
            db_ = new_wallet_db;
            current_ = cleanName;
            current_wallet_id_ = 1;  // Per-wallet DB always has id=1
            wallet_encrypted_ = false;
            wallet_locked_ = false;
            ensureWalletIdentityRow();

            struct Secret {
                std::string value;
                ~Secret() { secureClearString(value); }
            } owner_key{InitialOwnerKey(initial_master_seed)}, owner_plain;
            std::string identity;
            { auto lease = AcquireDatabaseLease(); identity = lease->EnsureDeliveryIdentity(); }
            owner_plain.value = "DNI01";
            owner_plain.value.push_back(kind == InitialSeedKind::Generated ? 1 : 0);
            if (identity.size() != 71 || identity.substr(0,7) != "DNWI01:")
                throw std::runtime_error("Initial wallet identity invalid");
            owner_plain.value += identity.substr(7);
            if (kind == InitialSeedKind::Generated) {
                std::array<uint8_t, 32> pq{};
                struct ClearPq { std::array<uint8_t,32>& value;
                    ~ClearPq() { OPENSSL_cleanse(value.data(),value.size()); } } clear_pq{pq};
                if (RAND_bytes(pq.data(), pq.size()) != 1)
                    throw std::runtime_error("Initial PQ master generation failed");
                owner_plain.value.append(reinterpret_cast<const char*>(pq.data()), pq.size());
                OPENSSL_cleanse(pq.data(), pq.size());
            }
            const auto sealed = encryptData(owner_plain.value, owner_key.value);
            const auto owner = util::hex(std::vector<uint8_t>(sealed.begin(), sealed.end()));
            if (!storeMasterSeedOwned(initial_master_seed, "", false, &owner)) {
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
            AdvanceDatabaseSession();
            db_ = previous_db;
            current_ = previous_current;
            current_wallet_id_ = previous_wallet_id;
            wallet_encrypted_ = previous_encrypted;
            wallet_locked_ = previous_locked;
            master_seed_ = previous_master_seed;
            secureClearBytes(previous_master_seed);
            throw;
        }

        AdvanceDatabaseSession();
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
    std::lock_guard<std::recursive_mutex> database_lock(database_lifecycle_mutex_);
    if (database_leases_ != 0) throw std::logic_error("Cannot switch wallet during database delivery");
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

    // From this point even a failed open may replace or clear the selection.
    AdvanceDatabaseSession();
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
    std::lock_guard<std::recursive_mutex> database_lock(database_lifecycle_mutex_);
    if (database_leases_ != 0) throw std::logic_error("Cannot rename wallet during database delivery");
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
        AdvanceDatabaseSession();
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

namespace {
// The existing issuance paths own the wallet lifecycle mutex. These SQL
// owners keep the issued address, key path and watched script indivisible.
void IssuanceCheck(sqlite3* db,int actual,int expected) {
    if(actual!=expected)throw std::runtime_error("Address issuance SQL failure: "+std::string(sqlite3_errmsg(db)));
}
struct IssuedStatement {
    sqlite3* db;
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> value{nullptr,sqlite3_finalize};
    IssuedStatement(sqlite3* database,const char* sql):db(database) {
        sqlite3_stmt* raw=nullptr;const int rc=sqlite3_prepare_v2(db,sql,-1,&raw,nullptr);value.reset(raw);
        IssuanceCheck(db,rc,SQLITE_OK);
    }
    void Int(int i,int64_t n){IssuanceCheck(db,sqlite3_bind_int64(value.get(),i,n),SQLITE_OK);}
    void Text(int i,const std::string& text){
        if(text.size()>INT_MAX)throw std::runtime_error("Address issuance text too large");
        IssuanceCheck(db,sqlite3_bind_text(value.get(),i,text.data(),int(text.size()),SQLITE_TRANSIENT),SQLITE_OK);
    }
    void Blob(int i,const void* bytes,int n){IssuanceCheck(db,sqlite3_bind_blob(value.get(),i,bytes,n,SQLITE_TRANSIENT),SQLITE_OK);}
    void Key(int i,const std::optional<wallet::KeyID>& key) {
        if(key)Blob(i,key->data(),int(key->size()));
        else IssuanceCheck(db,sqlite3_bind_null(value.get(),i),SQLITE_OK);
    }
    void Done(bool inserted=false){
        IssuanceCheck(db,sqlite3_step(value.get()),SQLITE_DONE);
        if(inserted && sqlite3_changes(db)!=1)throw std::runtime_error("Address issuance row not inserted");
    }
};
class IssuedAddressTransaction {
    sqlite3* db_;bool owned_=false;
    void Exec(const char* sql){IssuanceCheck(db_,sqlite3_exec(db_,sql,nullptr,nullptr,nullptr),SQLITE_OK);}
public:
    explicit IssuedAddressTransaction(sqlite3* db):db_(db) {
        if(!db_ || !sqlite3_get_autocommit(db_))throw std::runtime_error("Address issuance requires its own transaction");
        Exec("PRAGMA synchronous=FULL");
        { IssuedStatement policy(db_,"PRAGMA synchronous");
          IssuanceCheck(db_,sqlite3_step(policy.value.get()),SQLITE_ROW);
          if(sqlite3_column_int(policy.value.get(),0)!=2)throw std::runtime_error("Address issuance durability unavailable");
          policy.Done(); }
        Exec("BEGIN IMMEDIATE");owned_=true;
    }
    ~IssuedAddressTransaction(){
        if(owned_ && !sqlite3_get_autocommit(db_) &&
           sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK && !sqlite3_get_autocommit(db_))std::terminate();
    }
    void Commit(){Exec("COMMIT");owned_=false;}
    IssuedAddressTransaction(const IssuedAddressTransaction&)=delete;
    IssuedAddressTransaction& operator=(const IssuedAddressTransaction&)=delete;
};
bool IssuanceWalletColumn(sqlite3* db,const char* table) {
    const std::string sql="PRAGMA table_info("+std::string(table)+")";
    IssuedStatement query(db,sql.c_str());bool found=false;int rc;size_t rows=0;
    while((rc=sqlite3_step(query.value.get()))==SQLITE_ROW) {
        const auto* name=sqlite3_column_text(query.value.get(),1);
        if(!name)throw std::runtime_error("Address issuance schema unavailable");
        found|=std::string_view(reinterpret_cast<const char*>(name))=="wallet_id";++rows;
    }
    IssuanceCheck(db,rc,SQLITE_DONE);
    if(!rows)throw std::runtime_error("Address issuance table unavailable");
    return found;
}
void PersistIssuedAddress(sqlite3* db,int change,int index,const std::string& address,
        const std::string& label,const std::string& type,const std::string& script_hex,
        const std::vector<uint8_t>& script,const std::string& path,
        const std::optional<wallet::KeyID>& key,const std::optional<wallet::KeyID>& internal,
        const std::optional<wallet::KeyID>& output) {
    const auto now=std::time(nullptr);
    const bool wallet_column=IssuanceWalletColumn(db,"addresses");
    IssuedStatement row(db,wallet_column?
        "INSERT INTO addresses(wallet_id,account,change,idx,address,label,type,script_pubkey,key_id,internal_key_id,output_key_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)":
        "INSERT INTO addresses(account,change,idx,address,label,type,script_pubkey,key_id,internal_key_id,output_key_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    int n=1;if(wallet_column)row.Int(n++,1);
    row.Int(n++,0);row.Int(n++,change);row.Int(n++,index);row.Text(n++,address);row.Text(n++,label);
    row.Text(n++,type);row.Text(n++,script_hex);row.Key(n++,key);row.Key(n++,internal);row.Key(n++,output);row.Int(n++,now);row.Done(true);
    const bool path_wallet=IssuanceWalletColumn(db,"address_derivation_paths");
    IssuedStatement derived(db,path_wallet?
        "INSERT INTO address_derivation_paths(address,wallet_id,derivation_path,script_pubkey,account,change,address_index,created_at) VALUES(?,?,?,?,?,?,?,?)":
        "INSERT INTO address_derivation_paths(address,derivation_path,script_pubkey,account,change,address_index,created_at) VALUES(?,?,?,?,?,?,?)");
    n=1;derived.Text(n++,address);if(path_wallet)derived.Int(n++,1);
    derived.Text(n++,path);derived.Text(n++,script_hex);derived.Int(n++,0);derived.Int(n++,change);derived.Int(n++,index);derived.Int(n++,now);derived.Done(true);
    IssuedStatement watched(db,"INSERT OR IGNORE INTO watch_scripts(script_pubkey,path,is_change,last_seen_height,created_at) VALUES(?,?,?,0,?)");
    watched.Blob(1,script.data(),int(script.size()));watched.Text(2,path);watched.Int(3,change);watched.Int(4,now);watched.Done();
    // An existing watch row may be reused only with the exact issuance path.
    IssuedStatement check(db,"SELECT path,is_change FROM watch_scripts WHERE script_pubkey=?");check.Blob(1,script.data(),int(script.size()));
    IssuanceCheck(db,sqlite3_step(check.value.get()),SQLITE_ROW);
    const auto* stored=sqlite3_column_text(check.value.get(),0);
    if(sqlite3_column_type(check.value.get(),0)!=SQLITE_TEXT || !stored ||
       std::string(reinterpret_cast<const char*>(stored),sqlite3_column_bytes(check.value.get(),0))!=path ||
       sqlite3_column_type(check.value.get(),1)!=SQLITE_INTEGER || sqlite3_column_int64(check.value.get(),1)!=change)
        throw std::runtime_error("Address issuance watch ownership mismatch");
    check.Done();
}
// Validate explicit BIP84/BIP86 ownership claims without repairing records or
// publishing derived secrets. The caller owns the wallet transaction and seed.
std::optional<std::array<uint32_t,5>> AuthenticateHdInventory(
    sqlite3* db,const std::vector<uint8_t>& seed,const std::vector<uint8_t>* requested_script=nullptr) {
    const auto text=[](sqlite3_stmt* q,int col) {
        if(sqlite3_column_type(q,col)!=SQLITE_TEXT)throw std::runtime_error("HD inventory text type invalid");
        const auto* p=static_cast<const char*>(sqlite3_column_blob(q,col));
        const int n=sqlite3_column_bytes(q,col);
        if(!p || n<=0 || std::memchr(p,0,n))throw std::runtime_error("HD inventory text invalid");
        return std::string(p,n);
    };
    const auto integer=[](sqlite3_stmt* q,int col) {
        if(sqlite3_column_type(q,col)!=SQLITE_INTEGER)throw std::runtime_error("HD inventory integer type invalid");
        return sqlite3_column_int64(q,col);
    };
    const auto parse=[](const std::string& path) {
        std::array<uint32_t,5> parts{};size_t pos=2;
        if(path.compare(0,2,"m/")!=0)throw std::runtime_error("HD inventory path invalid");
        for(size_t i=0;i<parts.size();++i) {
            const auto start=pos;uint64_t value=0;
            while(pos<path.size() && path[pos]>='0' && path[pos]<='9') {
                value=value*10+uint32_t(path[pos++]-'0');
                if(value>0x7fffffff)throw std::runtime_error("HD inventory path range invalid");
            }
            if(pos==start)throw std::runtime_error("HD inventory path component missing");
            parts[i]=uint32_t(value);
            if(i<3 && (pos==path.size() || path[pos++]!='\''))throw std::runtime_error("HD inventory hardened path invalid");
            if(i<4 && (pos==path.size() || path[pos++]!='/'))throw std::runtime_error("HD inventory path separator invalid");
        }
        if(pos!=path.size() || (parts[0]!=84 && parts[0]!=86) || parts[3]>1)
            throw std::runtime_error("HD inventory path policy unsupported");
        return parts;
    };
    const auto script_text=[&](sqlite3_stmt* q,int col) {
        std::vector<uint8_t> bytes;
        if(!util::unhex(text(q,col),bytes) || bytes.empty())throw std::runtime_error("HD inventory script invalid");
        return bytes;
    };
    const bool address_wallet=IssuanceWalletColumn(db,"addresses");
    const bool path_wallet=IssuanceWalletColumn(db,"address_derivation_paths");
    const std::string sql=R"(SELECT p.address,p.derivation_path,p.script_pubkey,p.account,p.change,p.address_index,
        a.address,a.script_pubkey,a.account,a.change,a.idx,a.type,a.key_id,a.internal_key_id,a.output_key_id,)"+
        std::string(address_wallet?"a.wallet_id":"1")+","+std::string(path_wallet?"p.wallet_id":"1")+R"( FROM address_derivation_paths p
        LEFT JOIN addresses a ON a.address=p.address)"+
        std::string(requested_script?" WHERE lower(p.script_pubkey)=? OR lower(a.script_pubkey)=?":"")+" ORDER BY p.address";
    std::map<std::vector<uint8_t>,std::string> authenticated;
    IssuedStatement inventory(db,sql.c_str());int rc;
    std::optional<std::array<uint32_t,5>> requested_path;
    if(requested_script) {
        inventory.Text(1,util::hex(*requested_script));inventory.Text(2,util::hex(*requested_script));
    }
    while((rc=sqlite3_step(inventory.value.get()))==SQLITE_ROW) {
        auto* q=inventory.value.get();const auto address=text(q,0),path=text(q,1);const auto parts=parse(path);
        const auto recorded=script_text(q,2);
        if(text(q,6)!=address || script_text(q,7)!=recorded || integer(q,15)!=1 || integer(q,16)!=1 ||
           integer(q,3)!=parts[2] || integer(q,4)!=parts[3] || integer(q,5)!=parts[4] ||
           integer(q,8)!=parts[2] || integer(q,9)!=parts[3] || integer(q,10)!=parts[4])
            throw std::runtime_error("HD inventory address/path metadata mismatch");
        BIP32Deriver derive(seed.data(),seed.size());
        for(size_t i=0;i<3;++i)derive.deriveHardened(parts[i]);
        derive.deriveNormal(parts[3]);derive.deriveNormal(parts[4]);
        std::vector<uint8_t> program,script;
        wallet::KeyID primary{};std::optional<wallet::KeyID> internal_id,output_id;
        if(parts[0]==86) {
            const auto internal=derive.getXOnlyPubkey();std::array<uint8_t,32> output{};
            if(!TaprootKeys::ComputeTweakedPubkey(internal,output))throw std::runtime_error("HD inventory public derivation failed");
            program.assign(output.begin(),output.end());script={0x51,0x20};
            primary=wallet::ComputeKeyIDFromXOnly(internal);internal_id=primary;output_id=wallet::ComputeKeyIDFromXOnly(output);
        } else {
            const auto pub=derive.getCompressedPubkey();primary=wallet::ComputeKeyID(std::vector<uint8_t>(pub.begin(),pub.end()));
            program.assign(primary.begin(),primary.end());script={0x00,0x14};
        }
        script.insert(script.end(),program.begin(),program.end());
        bool address_matches=false;
        // Preserve recorded historical network encodings; this is ownership
        // authentication, not authorization to spend on a different network.
        for(const char* hrp:{"din","tdin","rdin"})
            address_matches|=address==bech32::Encode(hrp,parts[0]==86?1:0,program,
                parts[0]==86?bech32::Encoding::BECH32M:bech32::Encoding::BECH32);
        if(!address_matches || script!=recorded || text(q,11)!=(parts[0]==86?"p2tr":"p2wpkh"))
            throw std::runtime_error("HD inventory seed/address/script mismatch");
        const auto key_matches=[&](int col,const std::optional<wallet::KeyID>& expected) {
            // Older issuance predates KeyID columns. Authenticate every present
            // value, but do not invent or backfill an absent identifier.
            if(sqlite3_column_type(q,col)==SQLITE_NULL)return true;
            return expected && sqlite3_column_type(q,col)==SQLITE_BLOB && sqlite3_column_bytes(q,col)==20 &&
                sqlite3_column_blob(q,col) && CRYPTO_memcmp(sqlite3_column_blob(q,col),expected->data(),20)==0;
        };
        if(!key_matches(12,primary) || !key_matches(13,internal_id) || !key_matches(14,output_id))
            throw std::runtime_error("HD inventory key identifier mismatch");
        IssuedStatement watch(db,"SELECT path,is_change FROM watch_scripts WHERE script_pubkey=?");
        watch.Blob(1,script.data(),int(script.size()));IssuanceCheck(db,sqlite3_step(watch.value.get()),SQLITE_ROW);
        if(text(watch.value.get(),0)!=path || integer(watch.value.get(),1)!=parts[3])
            throw std::runtime_error("HD inventory watch binding mismatch");
        watch.Done();
        const auto [owner,inserted]=authenticated.emplace(script,path);
        if(!inserted && owner->second!=path)throw std::runtime_error("HD inventory conflicting script owner");
        if(requested_script) {
            if(script!=*requested_script || (requested_path && *requested_path!=parts))
                throw std::runtime_error("HD signing script owner mismatch");
            requested_path=parts;
        }
    }
    IssuanceCheck(db,rc,SQLITE_DONE);
    // Reverse-check explicit HD watch claims so deleting their path/address
    // companion cannot silently downgrade them to watch-only recognition.
    const std::string watch_sql=std::string("SELECT script_pubkey,path FROM watch_scripts")+
        (requested_script?" WHERE script_pubkey=?":"")+" ORDER BY script_pubkey";
    IssuedStatement watched(db,watch_sql.c_str());
    if(requested_script)watched.Blob(1,requested_script->data(),int(requested_script->size()));
    while((rc=sqlite3_step(watched.value.get()))==SQLITE_ROW) {
        auto* q=watched.value.get();const auto path=text(q,1);
        if(path.compare(0,4,"m/84")!=0 && path.compare(0,4,"m/86")!=0)continue;
        parse(path);
        if(sqlite3_column_type(q,0)!=SQLITE_BLOB || sqlite3_column_bytes(q,0)<=0 || !sqlite3_column_blob(q,0))
            throw std::runtime_error("HD inventory watch script invalid");
        const auto* p=static_cast<const uint8_t*>(sqlite3_column_blob(q,0));
        const std::vector<uint8_t> script(p,p+sqlite3_column_bytes(q,0));
        const auto found=authenticated.find(script);
        if(found==authenticated.end() || found->second!=path)throw std::runtime_error("HD inventory orphan watch claim");
    }
    IssuanceCheck(db,rc,SQLITE_DONE);
    return requested_path;
}
} // namespace

int WalletManager::getNextAddressIndex(int account, int change) const {
    std::lock_guard<std::recursive_mutex> ownership(database_lifecycle_mutex_);
    if(!db_ || current_wallet_id_==-1)throw std::runtime_error("No wallet is currently open");
    IssuedStatement query(db_,"SELECT COALESCE(MAX(idx),-1)+1 FROM addresses WHERE account=? AND change=?");
    query.Int(1,account);query.Int(2,change);IssuanceCheck(db_,sqlite3_step(query.value.get()),SQLITE_ROW);
    const auto next=sqlite3_column_int64(query.value.get(),0);
    if(sqlite3_column_type(query.value.get(),0)!=SQLITE_INTEGER || next<0 || next>INT_MAX)
        throw std::runtime_error("Address derivation index exhausted or invalid");
    query.Done();return int(next);
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
    std::lock_guard<std::recursive_mutex> database_lock(database_lifecycle_mutex_);
    if (database_leases_ != 0) throw std::logic_error("Cannot close wallet during database delivery");
    AdvanceDatabaseSession();
    if (utxo_index_) {
        utxo_index_->ClearRegisteredAddresses();
    }

    // Scrub sensitive material whenever a wallet is closed/destroyed.
    clearPrivateKeyCache();
    secureClearBytes(master_seed_);
    secureClearString(encryption_key_);
    OPENSSL_cleanse(pq_master_key_.data(), pq_master_key_.size());
    pq_master_key_loaded_ = false;
    // Viewing authority may survive wallet.lock so background scanning can
    // continue, but it must never survive closing/switching wallets. Retaining
    // either cache here would let the next wallet opened in this process scan
    // with the previous wallet's incoming or outgoing identity.
    for (auto& ivk : shielded_incoming_viewing_keys_) {
        OPENSSL_cleanse(ivk.data(), ivk.size());
    }
    for (auto& authority : shielded_recipient_viewing_authorities_) {
        OPENSSL_cleanse(&authority, sizeof(authority));
    }
    for (auto& ovk : shielded_outgoing_viewing_keys_) {
        OPENSSL_cleanse(ovk.data(), ovk.size());
    }
    shielded_incoming_viewing_keys_.clear();
    shielded_recipient_viewing_authorities_.clear();
    shielded_outgoing_viewing_keys_.clear();
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
    std::lock_guard<std::recursive_mutex> database_lock(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> database_lock(database_lifecycle_mutex_);
    if (database_leases_ != 0) throw std::logic_error("Cannot select wallet during database delivery");
    AdvanceDatabaseSession();
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

void WalletManager::AdvanceDatabaseSession() noexcept {
    // Exhaustion must not reuse an identity while a queued job still owns it.
    if (database_session_ == UINT64_MAX) std::terminate();
    ++database_session_;
}

WalletManager::DatabaseLease::DatabaseLease(WalletManager& owner)
    : owner_(owner), lock_(owner.database_lifecycle_mutex_),
      thread_(std::this_thread::get_id()), db_(owner.db_), name_(owner.current_),
      session_(owner.database_session_) {
    if (db_) {
        sqlite_mutex_ = sqlite3_db_mutex(db_);
        if (!sqlite_mutex_) throw std::runtime_error("Wallet database requires serialized SQLite");
        sqlite3_mutex_enter(sqlite_mutex_);
        if (!sqlite3_get_autocommit(db_) && owner_.database_leases_ == 0) {
            sqlite3_mutex_leave(sqlite_mutex_);
            throw std::runtime_error("Wallet database already has an active transaction");
        }
    }
    ++owner_.database_leases_;
}

std::string WalletManager::DatabaseLease::EnsureDeliveryIdentity() {
    if (thread_ != std::this_thread::get_id())
        throw std::logic_error("Wallet delivery identity used on another thread");
    if (!db_ || name_.empty() || !sqlite3_get_autocommit(db_))
        throw std::runtime_error("Wallet delivery identity ownership unavailable");
    struct Statement {
        sqlite3_stmt* stmt = nullptr;
        Statement(sqlite3* db, const char* sql) {
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                sqlite3_finalize(stmt);
                throw std::runtime_error("Wallet delivery identity prepare failed");
            }
        }
        ~Statement() { sqlite3_finalize(stmt); }
    };
    const auto sql = [&](const char* text) {
        if (sqlite3_exec(db_, text, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Wallet delivery identity SQL failed");
    };
    sql("PRAGMA synchronous=FULL");
    {
        Statement policy(db_, "PRAGMA synchronous");
        if (sqlite3_step(policy.stmt) != SQLITE_ROW || sqlite3_column_int(policy.stmt, 0) != 2 ||
            sqlite3_step(policy.stmt) != SQLITE_DONE)
            throw std::runtime_error("Wallet delivery identity durability unavailable");
    }
    sql("BEGIN IMMEDIATE"); // Failed BEGIN never adopts another transaction.
    try {
        { Statement selected(db_, "SELECT id FROM wallet_meta WHERE id=1");
          if (sqlite3_step(selected.stmt) != SQLITE_ROW || sqlite3_step(selected.stmt) != SQLITE_DONE)
              throw std::runtime_error("Wallet delivery identity metadata unavailable"); }
        bool column = false;
        { Statement columns(db_, "PRAGMA table_info(wallet_meta)");
          int rc;
          while ((rc = sqlite3_step(columns.stmt)) == SQLITE_ROW) {
              const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(columns.stmt, 1));
              if (name && std::string_view(name) == "runtime_delivery_id") column = true;
          }
          if (rc != SQLITE_DONE) throw std::runtime_error("Wallet delivery identity schema read failed"); }
        if (!column) sql("ALTER TABLE wallet_meta ADD COLUMN runtime_delivery_id BLOB");
        std::array<unsigned char, 32> id{};
        bool create = false;
        { Statement read(db_, "SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
          if (sqlite3_step(read.stmt) != SQLITE_ROW)
              throw std::runtime_error("Wallet delivery identity read failed");
          create = sqlite3_column_type(read.stmt, 0) == SQLITE_NULL;
          if (!create) {
              if (sqlite3_column_type(read.stmt, 0) != SQLITE_BLOB || sqlite3_column_bytes(read.stmt, 0) != int(id.size()))
                  throw std::runtime_error("Wallet delivery identity malformed");
              const auto* bytes = sqlite3_column_blob(read.stmt, 0);
              if (!bytes) throw std::runtime_error("Wallet delivery identity read failed");
              std::memcpy(id.data(), bytes, id.size());
          }
          if (sqlite3_step(read.stmt) != SQLITE_DONE)
              throw std::runtime_error("Wallet delivery identity read failed"); }
        if (create && RAND_bytes(id.data(), int(id.size())) != 1)
            throw std::runtime_error("Wallet delivery identity randomness unavailable");
        if (std::all_of(id.begin(), id.end(), [](auto byte) { return byte == 0; }))
            throw std::runtime_error("Wallet delivery identity malformed");
        if (create) {
            Statement write(db_, "UPDATE wallet_meta SET runtime_delivery_id=? WHERE id=1 AND runtime_delivery_id IS NULL");
            if (sqlite3_bind_blob(write.stmt, 1, id.data(), int(id.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_step(write.stmt) != SQLITE_DONE || sqlite3_changes(db_) != 1)
                throw std::runtime_error("Wallet delivery identity write failed");
        }
        static constexpr char hex[] = "0123456789abcdef";
        std::string result = "DNWI01:";
        for (auto byte : id) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        sql("COMMIT");
        return result; // No allocation or diagnostics after the durable commit.
    } catch (...) {
        if (!sqlite3_get_autocommit(db_) &&
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_)) std::terminate();
        throw;
    }
}

WalletManager::RecoverySeed::RecoverySeed(WalletManager& owner, std::span<const uint8_t> seed)
    : owner_(owner), thread_(std::this_thread::get_id()) {
    if (seed.size() != bytes_.size()) throw std::runtime_error("Wallet recovery seed unavailable");
    std::copy(seed.begin(), seed.end(), bytes_.begin());
    ++owner_.recovery_seeds_;
}
WalletManager::RecoverySeed::~RecoverySeed() noexcept {
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
    if (thread_ != std::this_thread::get_id() || owner_.recovery_seeds_ != 1)
        std::terminate();
    --owner_.recovery_seeds_;
}
std::unique_ptr<WalletManager::RecoverySeed>
WalletManager::DatabaseLease::CopyRecoverySeed(uint64_t expected_session) {
    if (thread_ != std::this_thread::get_id() || !db_ || name_.empty() ||
        expected_session != session_ || owner_.database_session_ != session_ || owner_.recovery_seeds_)
        throw std::runtime_error("Wallet recovery key ownership unavailable");
    owner_.checkUnlockTimeout();
    if (owner_.wallet_locked_ || owner_.master_seed_.size() != 64)
        throw std::runtime_error("Wallet recovery seed unavailable");
    return std::unique_ptr<RecoverySeed>(new RecoverySeed(owner_,owner_.master_seed_));
}

std::optional<SigningKey> WalletManager::DatabaseLease::ResolveSigningKey(
    const std::string& script_pubkey,const RecoverySeed& pin) {
    if(thread_!=std::this_thread::get_id() || pin.thread_!=thread_ ||
       &pin.owner_!=&owner_ || !db_ || db_!=owner_.db_ ||
       session_!=owner_.database_session_ || owner_.recovery_seeds_!=1)
        throw std::runtime_error("Signing key owner does not match wallet lease");
    std::vector<uint8_t> script;
    if(!util::unhex(script_pubkey,script) || script.empty())return std::nullopt;
    auto policy=script.size()==34 && script[0]==0x51 && script[1]==0x20
        ?SigningKeyPolicy::TaprootCanonical:SigningKeyPolicy::Untweaked;
    auto key=owner_.deriveKeyForScriptPubKeyOwned(script_pubkey,&policy,true);
    if(!key)return std::nullopt;
    return SigningKey(std::move(*key),std::move(script),policy);
}

namespace {
constexpr size_t kPaymentBytes=16*1024*1024;
constexpr size_t kPaymentCount=4096;
struct PaymentSecret {
    std::string value;
    ~PaymentSecret(){secureClearString(value);}
};
std::string PaymentKey(std::span<const uint8_t> seed) {
    if(seed.size()!=64)throw std::runtime_error("Pending payment seed unavailable");
    PaymentSecret material{"Dinero wallet pending payment owner v1"};
    material.value.append(reinterpret_cast<const char*>(seed.data()),seed.size());
    std::array<uint8_t,32> key{};
    ::SHA256(reinterpret_cast<const uint8_t*>(material.value.data()),material.value.size(),key.data());
    std::string out(reinterpret_cast<const char*>(key.data()),key.size());
    OPENSSL_cleanse(key.data(),key.size());return out;
}
bool PaymentColumn(sqlite3* db) {
    IssuedStatement q(db,"PRAGMA table_info(wallet_meta)");bool found=false;size_t rows=0;int rc;
    while((rc=sqlite3_step(q.value.get()))==SQLITE_ROW) {
        if(sqlite3_column_type(q.value.get(),1)!=SQLITE_TEXT)throw std::runtime_error("Payment schema unavailable");
        const auto* p=static_cast<const char*>(sqlite3_column_blob(q.value.get(),1));
        const int n=sqlite3_column_bytes(q.value.get(),1);
        if(!p || n<=0)throw std::runtime_error("Payment schema malformed");
        found|=std::string_view(p,n)=="pending_payment_owner";++rows;
    }
    IssuanceCheck(db,rc,SQLITE_DONE);
    if(!rows)throw std::runtime_error("Payment wallet metadata missing");
    return found;
}
std::string PaymentIdentity(sqlite3* db) {
    IssuedStatement q(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
    IssuanceCheck(db,sqlite3_step(q.value.get()),SQLITE_ROW);
    const auto* p=static_cast<const char*>(sqlite3_column_blob(q.value.get(),0));
    if(sqlite3_column_type(q.value.get(),0)!=SQLITE_BLOB || sqlite3_column_bytes(q.value.get(),0)!=32 || !p)
        throw std::runtime_error("Payment identity unavailable");
    std::string id(p,32);q.Done();
    if(std::all_of(id.begin(),id.end(),[](char c){return c==0;}))throw std::runtime_error("Payment identity invalid");
    return id;
}
void PaymentU64(std::string& out,uint64_t n) {
    for(int i=0;i<8;++i)out.push_back(char(n>>(8*i)));
}
void PaymentField(std::string& out,std::string_view bytes) {
    if(bytes.size()>kPaymentBytes-8 || out.size()>kPaymentBytes-bytes.size()-8)
        throw std::runtime_error("Pending payment capacity exceeded");
    PaymentU64(out,bytes.size());out.append(bytes);
}
void PaymentBlob(std::string& out,const std::vector<uint8_t>& bytes) {
    PaymentField(out,std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
}
struct PaymentReader {
    std::string_view bytes;size_t pos=0;
    uint64_t U64(){
        if(bytes.size()-pos<8)throw std::runtime_error("Payment framing incomplete");
        uint64_t n=0;for(int i=0;i<8;++i)n|=uint64_t(uint8_t(bytes[pos++]))<<(8*i);return n;
    }
    std::string Field(){
        const auto n=U64();if(n>bytes.size()-pos)throw std::runtime_error("Payment field incomplete");
        std::string out(bytes.substr(pos,size_t(n)));pos+=size_t(n);return out;
    }
    std::vector<uint8_t> Blob(){const auto s=Field();return {s.begin(),s.end()};}
};
uint64_t PaymentSum(uint64_t sum,uint64_t n) {
    if(n>MAX_SUPPLY_UNA_CONST || sum>MAX_SUPPLY_UNA_CONST-n)
        throw std::runtime_error("Payment amount range invalid");
    return sum+n;
}
std::vector<uint8_t> PaymentScript(const std::string& address) {
    // Use the shared address codec already linked by the wallet library.
    // Preserve the recorded network prefix; this is not network selection.
    for(const auto* hrp:{"din","tdin","rdin"}) {
        const auto parsed=DecodeWitnessAddress(address,hrp);
        if(parsed.is_valid && parsed.is_witness && !parsed.script_pubkey.empty())return parsed.script_pubkey;
    }
    throw std::runtime_error("Pending payment requires a valid witness address");
}
std::vector<PendingPaymentRecipient> PaymentRecipients(const PendingPaymentIntent& intent) {
    if (intent.additional_recipients.size() >= kPaymentCount)
        throw std::runtime_error("Payment recipient capacity exceeded");
    std::vector<PendingPaymentRecipient> result{{intent.address, intent.amount_una}};
    result.insert(result.end(), intent.additional_recipients.begin(), intent.additional_recipients.end());
    for (const auto& recipient : result) {
        if (recipient.address.empty() || recipient.address.find('\0') != std::string::npos || !recipient.amount_una)
            throw std::runtime_error("Payment recipient invalid");
    }
    return result;
}
uint64_t PaymentIntentTotal(const PendingPaymentIntent& intent) {
    uint64_t total = 0;
    for (const auto& recipient : PaymentRecipients(intent)) total = PaymentSum(total, recipient.amount_una);
    return total;
}
void ValidatePayment(const PendingPayment& p) {
    Transaction tx;size_t consumed=0;
    if(p.signed_body.empty() || p.signed_body.size()>kPaymentBytes ||
       !TransactionSerializer::Deserialize(tx,p.signed_body,consumed) || consumed!=p.signed_body.size() ||
       tx.Serialize(TxSerializationMode::WithWitness)!=p.signed_body ||
       tx.GetTxid().AsUint256().GetHex()!=p.txid || p.inputs.empty() || p.inputs.size()!=tx.vin.size() ||
       p.inputs.size()>kPaymentCount || p.created_at<=0 || p.intent.amount_una==0 ||
       p.intent.address.empty() || p.intent.address.find('\0')!=std::string::npos || p.intent.label.find('\0')!=std::string::npos)
        throw std::runtime_error("Pending payment body invalid");
    std::multiset<std::pair<std::vector<uint8_t>, uint64_t>> required;
    for (const auto& recipient : PaymentRecipients(p.intent))
        required.emplace(PaymentScript(recipient.address), recipient.amount_una);
    std::set<std::string> seen;uint64_t input=0,output=0;
    for(size_t i=0;i<p.inputs.size();++i) {
        const auto& coin=p.inputs[i];const auto& in=tx.vin[i];
        if(coin.txid!=in.prevout.txid.AsUint256().GetHex() || coin.vout!=in.prevout.vout || coin.script.empty() ||
           in.witness.empty() || !seen.insert(coin.txid+":"+std::to_string(coin.vout)).second)
            throw std::runtime_error("Payment input binding invalid");
        input=PaymentSum(input,coin.amount_una);
    }
    for(const auto& out:tx.vout) {
        if(out.is_confidential)throw std::runtime_error("Confidential payment owner unsupported");
        output=PaymentSum(output,out.value.GetUna());
        auto match = required.find({out.scriptPubKey, out.value.GetUna()});
        if (match != required.end()) required.erase(match);
    }
    if(!required.empty() || input<output || input-output!=p.fee_una)
        throw std::runtime_error("Payment intent or fee mismatch");
    (void)PaymentSum(PaymentIntentTotal(p.intent),p.fee_una);
}
std::string EncodePayments(const std::string& identity,const std::vector<PendingPayment>& records) {
    if(records.empty() || records.size()>kPaymentCount)throw std::runtime_error("Payment count invalid");
    const bool batch = std::any_of(records.begin(), records.end(), [](const auto& p) {
        return !p.intent.additional_recipients.empty();
    });
    std::string out=batch?"DNPP02":"DNPP01";PaymentField(out,identity);PaymentU64(out,records.size());
    std::set<std::string> txids,reservations;
    for(const auto& p:records) {
        ValidatePayment(p);
        if(!txids.insert(p.txid).second)throw std::runtime_error("Duplicate payment body");
        PaymentField(out,p.txid);PaymentBlob(out,p.signed_body);PaymentField(out,p.intent.address);
        PaymentU64(out,p.intent.amount_una);PaymentField(out,p.intent.label);
        if (batch) {
            PaymentU64(out, p.intent.additional_recipients.size());
            for (const auto& recipient : p.intent.additional_recipients) {
                PaymentField(out, recipient.address);PaymentU64(out, recipient.amount_una);
            }
        }
        PaymentU64(out,p.fee_una);
        PaymentU64(out,uint64_t(p.created_at));PaymentU64(out,p.inputs.size());
        for(const auto& in:p.inputs) {
            if(!reservations.insert(in.txid+":"+std::to_string(in.vout)).second)
                throw std::runtime_error("Payment input already reserved");
            PaymentField(out,in.txid);PaymentU64(out,in.vout);PaymentU64(out,in.amount_una);PaymentBlob(out,in.script);
        }
        if(out.size()>kPaymentBytes)throw std::runtime_error("Pending payment capacity exceeded");
    }
    return out;
}
std::vector<PendingPayment> DecodePayments(const std::string& plain,const std::string& identity) {
    const bool batch = plain.substr(0,6)=="DNPP02";
    if(plain.size()>kPaymentBytes || (!batch && plain.substr(0,6)!="DNPP01"))throw std::runtime_error("Payment format invalid");
    PaymentReader r{std::string_view(plain).substr(6)};
    if(r.Field()!=identity)throw std::runtime_error("Payment belongs to another wallet");
    const auto n=r.U64();if(!n || n>kPaymentCount)throw std::runtime_error("Payment count invalid");
    std::vector<PendingPayment> result;
    for(uint64_t i=0;i<n;++i) {
        PendingPayment p;p.txid=r.Field();p.signed_body=r.Blob();p.intent.address=r.Field();
        p.intent.amount_una=r.U64();p.intent.label=r.Field();
        if (batch) {
            const auto count = r.U64();
            if (count >= kPaymentCount) throw std::runtime_error("Payment recipient capacity exceeded");
            for (uint64_t j = 0; j < count; ++j) p.intent.additional_recipients.push_back({r.Field(), r.U64()});
        }
        p.fee_una=r.U64();
        auto time=r.U64();if(time>INT64_MAX)throw std::runtime_error("Payment time invalid");p.created_at=int64_t(time);
        const auto count=r.U64();if(!count || count>kPaymentCount)throw std::runtime_error("Payment inputs invalid");
        for(uint64_t j=0;j<count;++j) {
            PendingPaymentInput in;in.txid=r.Field();const auto index=r.U64();
            if(index>UINT32_MAX)throw std::runtime_error("Payment output index invalid");in.vout=uint32_t(index);
            in.amount_una=r.U64();in.script=r.Blob();p.inputs.push_back(std::move(in));
        }
        result.push_back(std::move(p));
    }
    if(r.pos!=r.bytes.size())throw std::runtime_error("Trailing payment data");
    PaymentSecret canonical{EncodePayments(identity,result)};
    if(canonical.value!=plain)throw std::runtime_error("Noncanonical payment owner");
    return result;
}
} // namespace

std::vector<PendingPayment> WalletManager::ReadPendingPaymentsOwned(std::span<const uint8_t> seed) const {
    if(!db_ || sqlite3_get_autocommit(db_))throw std::logic_error("Payment read requires owned transaction");
    if(!PaymentColumn(db_))return {};
    IssuedStatement q(db_,"SELECT pending_payment_owner FROM wallet_meta WHERE id=1");
    IssuanceCheck(db_,sqlite3_step(q.value.get()),SQLITE_ROW);
    const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.value.get(),0));
    const int size=sqlite3_column_bytes(q.value.get(),0);
    if(sqlite3_column_type(q.value.get(),0)!=SQLITE_BLOB || !bytes || size<28 || size>int(kPaymentBytes+28))
        throw std::runtime_error("Pending payment owner missing or malformed");
    const std::string sealed(bytes,size);q.Done();
    PaymentSecret key{PaymentKey(seed)},plain{decryptData(sealed,key.value)};
    auto records=DecodePayments(plain.value,PaymentIdentity(db_));
    for(const auto& p:records) {
        IssuedStatement history(db_,"SELECT address,amount,category,label,time FROM transactions WHERE wallet_id=? AND txid=?");
        history.Int(1,current_wallet_id_);history.Text(2,p.txid);
        IssuanceCheck(db_,sqlite3_step(history.value.get()),SQLITE_ROW);
        const auto text=[&](int col,const std::string& expected) {
            const auto* bytes=static_cast<const char*>(sqlite3_column_blob(history.value.get(),col));
            const int n=sqlite3_column_bytes(history.value.get(),col);
            return sqlite3_column_type(history.value.get(),col)==SQLITE_TEXT && n==int(expected.size()) &&
                (n==0 || (bytes && std::memcmp(bytes,expected.data(),size_t(n))==0));
        };
        if(!text(0,p.intent.address) || !text(2,"send") || !text(3,p.intent.label) ||
           sqlite3_column_type(history.value.get(),1)!=SQLITE_FLOAT ||
           sqlite3_column_double(history.value.get(),1)!=-static_cast<double>(PaymentSum(PaymentIntentTotal(p.intent),p.fee_una))/1e8 ||
           sqlite3_column_type(history.value.get(),4)!=SQLITE_INTEGER || sqlite3_column_int64(history.value.get(),4)!=p.created_at)
            throw std::runtime_error("Pending payment history differs from retained intent");
        history.Done();
    }
    return records;
}

std::vector<PendingPayment> WalletManager::getPendingPayments() const {
    auto& self=const_cast<WalletManager&>(*this);auto lease=self.AcquireDatabaseLease();
    if(!db_)throw std::runtime_error("Wallet not loaded");
    IssuedAddressTransaction transaction(db_);
    if(!PaymentColumn(db_)){transaction.Commit();return {};}
    auto pin=lease->CopyRecoverySeed(lease->Session());
    auto records=ReadPendingPaymentsOwned(pin->Bytes());transaction.Commit();return records;
}

void WalletManager::DatabaseLease::StagePayment(const RecoverySeed& pin,const UnsignedTransaction& input,
                                               const Transaction& signed_tx,const PendingPaymentIntent& intent) {
    if(thread_!=std::this_thread::get_id() || pin.thread_!=thread_ || &pin.owner_!=&owner_ ||
       !db_ || db_!=owner_.db_ || session_!=owner_.database_session_ || owner_.recovery_seeds_!=1)
        throw std::runtime_error("Payment owner does not match wallet lease");
    if(input.tx.Serialize(TxSerializationMode::WithoutWitness)!=signed_tx.Serialize(TxSerializationMode::WithoutWitness) ||
       input.selected_utxos.size()!=signed_tx.vin.size())throw std::runtime_error("Payment signing body changed");
    // Exact explicit recipients plus at most one authenticated change output.
    // This API never replaces another retained body or reuses its reservations.
    const auto recipients=PaymentRecipients(intent);std::vector<uint8_t> change;
    if(input.change_amount) {
        change=PaymentScript(input.change_address);
        if(!ResolveSigningKey(util::hex(change),pin))
            throw std::runtime_error("Payment change owner unavailable");
    }
    std::vector<std::pair<std::vector<uint8_t>,uint64_t>> expected,actual;
    for (const auto& recipient : recipients) expected.emplace_back(PaymentScript(recipient.address), recipient.amount_una);
    if(input.change_amount)expected.emplace_back(change,input.change_amount);
    for(const auto& out:signed_tx.vout)actual.emplace_back(out.scriptPubKey,out.value.GetUna());
    std::sort(expected.begin(),expected.end());std::sort(actual.begin(),actual.end());
    if(expected!=actual)throw std::runtime_error("Payment outputs differ from authorized intent");
    PendingPayment p;p.txid=signed_tx.GetTxid().AsUint256().GetHex();p.signed_body=signed_tx.Serialize(TxSerializationMode::WithWitness);
    p.intent=intent;p.created_at=std::time(nullptr);uint64_t total=0,outputs=0;
    for(const auto& c:input.selected_utxos) {
        if(c.is_confidential)throw std::runtime_error("Confidential payment input unsupported");
        p.inputs.push_back({c.GetTxIdHex(),c.vout,c.value.GetUna(),c.spk});total=PaymentSum(total,c.value.GetUna());
    }
    for(const auto& out:signed_tx.vout)outputs=PaymentSum(outputs,out.value.GetUna());
    if(total<outputs)throw std::runtime_error("Payment outputs exceed inputs");p.fee_una=total-outputs;
    if(p.fee_una!=input.fee)throw std::runtime_error("Payment fee differs from signed builder result");
    ValidatePayment(p);
    (void)EnsureDeliveryIdentity(); // Separate existing identity prerequisite, not a recovery certificate.
    IssuedAddressTransaction transaction(db_);
    const bool installed=PaymentColumn(db_);
    auto records=owner_.ReadPendingPaymentsOwned(pin.Bytes());
    for(const auto& c:p.inputs) {
        if(owner_.locked_utxos_.count(c.txid+":"+std::to_string(c.vout)))throw std::runtime_error("Payment input manually locked");
        IssuedStatement q(db_,"SELECT amount,script_pubkey,is_spent FROM utxos WHERE wallet_id=? AND txid=? AND vout=?");
        q.Int(1,owner_.current_wallet_id_);q.Text(2,c.txid);q.Int(3,c.vout);
        IssuanceCheck(db_,sqlite3_step(q.value.get()),SQLITE_ROW);
        if(sqlite3_column_type(q.value.get(),0)!=SQLITE_INTEGER || sqlite3_column_int64(q.value.get(),0)<0 ||
           uint64_t(sqlite3_column_int64(q.value.get(),0))!=c.amount_una || sqlite3_column_type(q.value.get(),1)!=SQLITE_TEXT ||
           sqlite3_column_type(q.value.get(),2)!=SQLITE_INTEGER || sqlite3_column_int64(q.value.get(),2)!=0)
            throw std::runtime_error("Payment selected coin no longer available");
        const auto* text=static_cast<const char*>(sqlite3_column_blob(q.value.get(),1));const int size=sqlite3_column_bytes(q.value.get(),1);
        std::vector<uint8_t> script;
        if(!text || size<=0 || !util::unhex(std::string(text,size),script) || script!=c.script)
            throw std::runtime_error("Payment selected coin script changed");
        q.Done();
    }
    // Exact retries are observable through getPendingPayments; they do not
    // silently sign another body or replace the retained operation's history.
    records.push_back(p);
    PaymentSecret plain{EncodePayments(PaymentIdentity(db_),records)},key{PaymentKey(pin.Bytes())};
    const auto sealed=owner_.encryptData(plain.value,key.value);
    if(!installed)IssuanceCheck(db_,sqlite3_exec(db_,"ALTER TABLE wallet_meta ADD COLUMN pending_payment_owner BLOB",nullptr,nullptr,nullptr),SQLITE_OK);
    IssuedStatement save(db_,"UPDATE wallet_meta SET pending_payment_owner=? WHERE id=1");save.Blob(1,sealed.data(),int(sealed.size()));save.Done(true);
    IssuedStatement history(db_,"INSERT INTO transactions(wallet_id,txid,address,amount,confirmations,category,label,time,is_coinbase,height) VALUES(?,?,?,?,0,'send',?,?,0,0)");
    history.Int(1,owner_.current_wallet_id_);history.Text(2,p.txid);history.Text(3,p.intent.address);
    const auto spent=PaymentSum(PaymentIntentTotal(p.intent),p.fee_una);
    IssuanceCheck(db_,sqlite3_bind_double(history.value.get(),4,-static_cast<double>(spent)/1e8),SQLITE_OK);
    history.Text(5,p.intent.label);history.Int(6,p.created_at);history.Done(true);
    transaction.Commit();
}

WalletManager::DatabaseLease::~DatabaseLease() noexcept {
    if (thread_ != std::this_thread::get_id()) std::terminate();
    if (owner_.database_leases_ == 1 && owner_.recovery_seeds_) std::terminate();
    // Entry was in autocommit and no other thread can use this connection.
    // An unfinished transaction therefore belongs to this lease group, never
    // an unrelated caller. Nested same-thread operations must not roll back
    // the outer job; the last lease prevents it escaping into the next job.
    if (owner_.database_leases_ == 1 && db_ && !sqlite3_get_autocommit(db_)) {
        if (sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_)) std::terminate();
    }
    --owner_.database_leases_;
    if (sqlite_mutex_) sqlite3_mutex_leave(sqlite_mutex_);
}

std::unique_ptr<WalletManager::DatabaseLease> WalletManager::AcquireDatabaseLease() {
    return std::unique_ptr<DatabaseLease>(new DatabaseLease(*this));
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
void WalletManager::rewriteEncryptionPolicy(const std::string &old_passphrase,
                                            const std::string &new_passphrase, bool encrypted) {
    auto lease = AcquireDatabaseLease();
    if (!lease || !db_ || current_wallet_id_ < 0 || recovery_seeds_)
        throw std::runtime_error("Wallet encryption owner unavailable");
    if (encrypted && new_passphrase.empty())
        throw std::runtime_error("Passphrase cannot be empty");
    IssuedAddressTransaction transaction(db_);
    struct Secret {
        std::string value;
        ~Secret() { secureClearString(value); }
    } old_key, new_key, pq_plain;
    struct Seed {
        std::vector<uint8_t> value;
        ~Seed() { secureClearBytes(value); }
    } seed;
    const bool was_encrypted = wallet_encrypted_;
    const auto read_text = [](sqlite3_stmt *q, int col) {
        if (sqlite3_column_type(q, col) != SQLITE_TEXT)
            throw std::runtime_error("Invalid encryption text field");
        const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(q, col));
        const int n = sqlite3_column_bytes(q, col);
        if (!p || n < 0)
            throw std::runtime_error("Missing encryption text field");
        return std::string(p, n);
    };
    const auto setting = [&](const char *name) -> std::optional<std::string> {
        IssuedStatement q(db_, "SELECT value FROM settings WHERE key=?");
        q.Text(1, name);
        const int rc = sqlite3_step(q.value.get());
        if (rc == SQLITE_DONE)
            return std::nullopt;
        IssuanceCheck(db_, rc, SQLITE_ROW);
        auto value = read_text(q.value.get(), 0);
        q.Done();
        return value;
    };
    const auto policy = setting("wallet_encrypted");
    if ((policy && *policy != (was_encrypted ? "1" : "0")) || (!policy && was_encrypted))
        throw std::runtime_error("Wallet encryption policy mismatch");
    {
        IssuedStatement q(db_, "SELECT encrypted FROM encryption_metadata WHERE id=1");
        const int rc = sqlite3_step(q.value.get());
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_type(q.value.get(), 0) != SQLITE_INTEGER ||
                sqlite3_column_int64(q.value.get(), 0) != (was_encrypted ? 1 : 0))
                throw std::runtime_error("Wallet encryption metadata mismatch");
            q.Done();
        } else {
            IssuanceCheck(db_, rc, SQLITE_DONE);
            if (was_encrypted)
                throw std::runtime_error("Wallet encryption metadata missing");
        }
    }
    if (was_encrypted) {
        const auto salt = setting("wallet_salt"), verify = setting("wallet_verify_hash");
        std::vector<uint8_t> salt_bytes, expected;
        if (!salt || !verify || !util::unhex(*salt, salt_bytes) || salt_bytes.size() != 32 ||
            !util::unhex(*verify, expected) || expected.size() != 32)
            throw std::runtime_error("Wallet encryption credentials missing");
        const std::string binary_salt(reinterpret_cast<const char *>(salt_bytes.data()),
                                      salt_bytes.size());
        old_key.value = deriveKey(old_passphrase, binary_salt);
        const auto verified = [&] {
            std::array<uint8_t, 32> hash{};
            ::sha256(reinterpret_cast<const uint8_t *>(old_key.value.data()), old_key.value.size(),
                     hash.data());
            return CRYPTO_memcmp(hash.data(), expected.data(), 32) == 0;
        };
        if (!verified()) {
            secureClearString(old_key.value);
            old_key.value = deriveKeyLegacy(old_passphrase, binary_salt);
            if (!verified())
                throw std::runtime_error("Invalid passphrase");
        }
        if (!wallet_locked_ && !encryption_key_.empty() && old_key.value != encryption_key_)
            throw std::runtime_error("Live encryption owner mismatch");
    }
    auto loaded = loadMasterSeed(was_encrypted ? old_passphrase : "");
    if (!loaded || loaded->size() != 64)
        throw std::runtime_error("Existing wallet seed unavailable");
    seed.value = std::move(*loaded);
    (void)ReadPendingPaymentsOwned(seed.value);
    if (!master_seed_.empty() && !ConstantTimeEqual(seed.value, master_seed_))
        throw std::runtime_error("Existing wallet seed mismatch");
    const auto pq = setting("v7_pq_master_key_encrypted");
    if (pq && !pq->empty()) {
        // There is no existing unencrypted P2MR master-key representation.
        // Refuse that policy change before effects instead of losing its owner.
        if (!was_encrypted || !encrypted)
            throw std::runtime_error("P2MR master key requires encrypted wallet policy");
        std::vector<uint8_t> bytes;
        if (!util::unhex(*pq, bytes) || bytes.size() != 60)
            throw std::runtime_error("P2MR master key wrapper invalid");
        pq_plain.value = decryptData(
            std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), old_key.value);
        if (pq_plain.value.size() != 32)
            throw std::runtime_error("P2MR master key invalid");
        if (pq_master_key_loaded_ &&
            CRYPTO_memcmp(pq_plain.value.data(), pq_master_key_.data(), 32) != 0)
            throw std::runtime_error("Live P2MR master key mismatch");
    }
    std::string salt_hex, verify_hex;
    if (encrypted) {
        std::array<uint8_t, 32> salt{}, verify{};
        if (RAND_bytes(salt.data(), salt.size()) != 1)
            throw std::runtime_error("Encryption salt generation failed");
        new_key.value = deriveKey(
            new_passphrase, std::string(reinterpret_cast<const char *>(salt.data()), salt.size()));
        ::sha256(reinterpret_cast<const uint8_t *>(new_key.value.data()), new_key.value.size(),
                 verify.data());
        salt_hex = util::hex(std::vector<uint8_t>(salt.begin(), salt.end()));
        verify_hex = util::hex(std::vector<uint8_t>(verify.begin(), verify.end()));
    }
    // Prepare all view authority while failure can still roll back, keeping the
    // same seed and authenticated account identity across the policy change.
    std::vector<ShieldedIncomingViewingKey> incoming, outgoing;
    std::vector<ShieldedRecipientViewingAuthority> recipients;
    struct ClearViews {
        std::vector<ShieldedIncomingViewingKey> &incoming;
        std::vector<ShieldedIncomingViewingKey> &outgoing;
        std::vector<ShieldedRecipientViewingAuthority> &recipients;
        ~ClearViews() {
            if (!incoming.empty())
                OPENSSL_cleanse(incoming.data(), incoming.size() * sizeof(incoming[0]));
            if (!outgoing.empty())
                OPENSSL_cleanse(outgoing.data(), outgoing.size() * sizeof(outgoing[0]));
            if (!recipients.empty())
                OPENSSL_cleanse(recipients.data(), recipients.size() * sizeof(recipients[0]));
        }
    } clear_views{incoming, outgoing, recipients};
    for (uint32_t account = 0; account < 4; ++account) {
        auto keys =
            wallet::shielded::DeriveShieldedAccount(seed.value.data(), seed.value.size(), account);
        ScopedShieldedAccountKeys guard(keys);
        incoming.push_back(keys.ivk);
        outgoing.push_back(keys.ovk);
        recipients.push_back({keys.ivk, keys.ak, keys.nvk});
    }
    const auto validate_scalar = [](const std::string &bytes) {
        if (bytes.size() != 32)
            throw std::runtime_error("Invalid imported key length");
        struct Key {
            std::array<uint8_t, 32> value{};
            ~Key() { OPENSSL_cleanse(value.data(), value.size()); }
        } key;
        std::copy(bytes.begin(), bytes.end(), key.value.begin());
        std::array<uint8_t, 32> pub{};
        int parity = 0;
        if (!TaprootKeys::DeriveXOnlyPubkey(key.value, pub, parity))
            throw std::runtime_error("Invalid imported private key");
        return pub;
    };
    bool taproot = false;
    {
        IssuedStatement q(db_,
                          "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='taproot_keys'");
        const int rc = sqlite3_step(q.value.get());
        if (rc == SQLITE_ROW) {
            taproot = true;
            q.Done();
        } else
            IssuanceCheck(db_, rc, SQLITE_DONE);
    }
    if (taproot) {
        IssuedStatement q(
            db_,
            "SELECT address,internal_privkey,internal_pubkey,output_pubkey,is_privkey_encrypted "
            "FROM taproot_keys ORDER BY address");
        int rc;
        while ((rc = sqlite3_step(q.value.get())) == SQLITE_ROW) {
            const auto address = read_text(q.value.get(), 0);
            auto *row = q.value.get();
            if (sqlite3_column_type(row, 1) != SQLITE_BLOB ||
                sqlite3_column_bytes(row, 1) != (was_encrypted ? 60 : 32) ||
                sqlite3_column_type(row, 4) != SQLITE_INTEGER ||
                sqlite3_column_int64(row, 4) != (was_encrypted ? 1 : 0))
                throw std::runtime_error("Imported encryption policy mismatch");
            Secret plain, stored;
            const auto *raw = static_cast<const char *>(sqlite3_column_blob(row, 1));
            if (!raw)
                throw std::runtime_error("Imported key missing");
            stored.value.assign(raw, sqlite3_column_bytes(row, 1));
            plain.value = was_encrypted ? decryptData(stored.value, old_key.value) : stored.value;
            const auto internal = validate_scalar(plain.value);
            std::array<uint8_t, 32> output{};
            if (!TaprootKeys::ComputeTweakedPubkey(internal, output))
                throw std::runtime_error("Imported output invalid");
            for (int col : {2, 3}) {
                if (sqlite3_column_type(row, col) != SQLITE_BLOB ||
                    sqlite3_column_bytes(row, col) != 32 || !sqlite3_column_blob(row, col) ||
                    CRYPTO_memcmp(sqlite3_column_blob(row, col),
                                  col == 2 ? internal.data() : output.data(), 32) != 0)
                    throw std::runtime_error("Imported public binding mismatch");
            }
            const auto &network = Params().name;
            if (address != TaprootKeys::CreateTaprootAddress(output, network == "regtest" ? "rdin"
                                                                     : network == "testnet"
                                                                         ? "tdin"
                                                                         : "din"))
                throw std::runtime_error("Imported address binding mismatch");
            Secret replacement;
            replacement.value = encrypted ? encryptData(plain.value, new_key.value) : plain.value;
            secureClearString(stored.value);
            stored.value.swap(replacement.value);
            IssuedStatement write(db_, "UPDATE taproot_keys SET "
                                       "internal_privkey=?,is_privkey_encrypted=? WHERE address=?");
            write.Blob(1, stored.value.data(), int(stored.value.size()));
            write.Int(2, encrypted ? 1 : 0);
            write.Text(3, address);
            write.Done(true);
        }
        IssuanceCheck(db_, rc, SQLITE_DONE);
    }
    {
        IssuedStatement q(db_,
                          "SELECT address,private_key_enc FROM imported_keys ORDER BY address");
        int rc;
        while ((rc = sqlite3_step(q.value.get())) == SQLITE_ROW) {
            const auto address = read_text(q.value.get(), 0);
            Secret stored, plain;
            stored.value = read_text(q.value.get(), 1);
            if (was_encrypted) {
                plain.value = decryptData(stored.value, old_key.value);
                if (plain.value.size() == 32)
                    validate_scalar(plain.value);
                else {
                    std::vector<uint8_t> bytes;
                    if (plain.value.size() != 64 || !util::unhex(plain.value, bytes))
                        throw std::runtime_error("Legacy imported key invalid");
                    Secret decoded;
                    decoded.value.assign(reinterpret_cast<const char *>(bytes.data()),
                                         bytes.size());
                    secureClearBytes(bytes);
                    validate_scalar(decoded.value);
                }
            } else {
                std::vector<uint8_t> bytes;
                if (stored.value.size() != 64 || !util::unhex(stored.value, bytes))
                    throw std::runtime_error("Legacy imported key invalid");
                Secret decoded;
                decoded.value.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                secureClearBytes(bytes);
                validate_scalar(decoded.value);
                plain.value = stored.value;
            }
            if (encrypted) {
                Secret replacement;
                replacement.value = encryptData(plain.value, new_key.value);
                secureClearString(stored.value);
                stored.value.swap(replacement.value);
            } else if (plain.value.size() == 32) {
                Seed decoded;
                decoded.value.assign(plain.value.begin(), plain.value.end());
                stored.value = util::hex(decoded.value);
            } else
                stored.value = plain.value;
            IssuedStatement write(db_,
                                  "UPDATE imported_keys SET private_key_enc=? WHERE address=?");
            write.Text(1, stored.value);
            write.Text(2, address);
            write.Done(true);
        }
        IssuanceCheck(db_, rc, SQLITE_DONE);
    }
    // Preserve the existing hd_seeds v2 format, without invoking seed replacement
    // or publishing its in-memory cache before the transaction commits.
    std::array<uint8_t, 32> seed_salt{};
    if (RAND_bytes(seed_salt.data(), seed_salt.size()) != 1)
        throw std::runtime_error("Seed salt generation failed");
    struct Derived {
        std::array<uint8_t, 64> value{};
        ~Derived() { OPENSSL_cleanse(value.data(), value.size()); }
    } derived;
    const std::string_view pass = encrypted ? std::string_view(new_passphrase) : std::string_view{};
    dinero::crypto::PBKDF2_HMAC_SHA512(reinterpret_cast<const uint8_t *>(pass.data()), pass.size(),
                                       seed_salt.data(), seed_salt.size(), 600000,
                                       derived.value.data(), derived.value.size());
    Secret seed_key, seed_plain;
    seed_key.value.assign(reinterpret_cast<const char *>(derived.value.data()), 32);
    seed_plain.value.assign(reinterpret_cast<const char *>(seed.value.data()), seed.value.size());
    const auto sealed = encryptData(seed_plain.value, seed_key.value);
    std::string blob(reinterpret_cast<const char *>(seed_salt.data()), seed_salt.size());
    blob += sealed;
    {
        IssuedStatement write(
            db_, "UPDATE hd_seeds SET encrypted_seed=?,salt=?,encryption_version=2 WHERE id=1");
        write.Blob(1, blob.data(), int(blob.size()));
        write.Blob(2, seed_salt.data(), 32);
        write.Done(true);
    }
    const auto set = [&](const char *name, const std::string &value) {
        IssuedStatement q(db_, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO "
                               "UPDATE SET value=excluded.value");
        q.Text(1, name);
        q.Text(2, value);
        q.Done(true);
    };
    set("wallet_encrypted", encrypted ? "1" : "0");
    set("wallet_salt", salt_hex);
    set("wallet_verify_hash", verify_hex);
    if (!pq_plain.value.empty()) {
        const auto value = encryptData(pq_plain.value, new_key.value);
        set("v7_pq_master_key_encrypted",
            util::hex(std::vector<uint8_t>(value.begin(), value.end())));
    }
    {
        IssuedStatement meta(
            db_,
            "INSERT INTO "
            "encryption_metadata(id,encrypted,kdf,kdf_iterations,cipher,salt,created_at,updated_at)"
            " VALUES(1,?,'pbkdf2-hmac-sha512',600000,'AES-256-GCM',?,strftime('%s','now'),strftime("
            "'%s','now')) ON CONFLICT(id) DO UPDATE SET "
            "encrypted=excluded.encrypted,kdf=excluded.kdf,kdf_iterations=excluded.kdf_iterations,"
            "cipher=excluded.cipher,salt=excluded.salt,updated_at=excluded.updated_at");
        meta.Int(1, encrypted ? 1 : 0);
        meta.Blob(2, seed_salt.data(), 32);
        meta.Done(true);
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const bool unlocked =
        !encrypted ||
        (was_encrypted && !wallet_locked_ &&
         (unlock_timeout_ <= 0 || unlock_time_ <= 0 || now - unlock_time_ < unlock_timeout_));
    transaction.Commit();
    // Nothing fallible remains: publish only the committed policy, preserving
    // seed-derived account identities and clearing old plaintext caches.
    for (auto &item : private_key_cache_)
        secureClearBytes(item.second);
    private_key_cache_.clear();
    secureClearString(encryption_key_);
    secureClearBytes(master_seed_);
    OPENSSL_cleanse(pq_master_key_.data(), pq_master_key_.size());
    pq_master_key_loaded_ = false;
    wallet_encrypted_ = encrypted;
    wallet_locked_ = !unlocked;
    if (unlocked) {
        master_seed_.swap(seed.value);
        if (encrypted)
            encryption_key_.swap(new_key.value);
        if (!pq_plain.value.empty()) {
            std::memcpy(pq_master_key_.data(), pq_plain.value.data(), 32);
            pq_master_key_loaded_ = true;
        }
    }
    if (!encrypted || !unlocked) {
        unlock_time_ = 0;
        unlock_timeout_ = 0;
    }
    shielded_incoming_viewing_keys_.swap(incoming);
    shielded_outgoing_viewing_keys_.swap(outgoing);
    shielded_recipient_viewing_authorities_.swap(recipients);
}

void WalletManager::encryptWallet(const std::string &passphrase) {
    std::lock_guard<std::recursive_mutex> owner(database_lifecycle_mutex_);
    if (wallet_encrypted_)
        throw std::runtime_error("Wallet is already encrypted");
    rewriteEncryptionPolicy("", passphrase, true);
}
void WalletManager::decryptWallet(const std::string &passphrase) {
    std::lock_guard<std::recursive_mutex> owner(database_lifecycle_mutex_);
    if (!wallet_encrypted_)
        throw std::runtime_error("Wallet is not encrypted");
    rewriteEncryptionPolicy(passphrase, "", false);
}
void WalletManager::changePassphrase(const std::string &oldPassphrase,
                                     const std::string &newPassphrase) {
    std::lock_guard<std::recursive_mutex> owner(database_lifecycle_mutex_);
    if (!wallet_encrypted_)
        throw std::runtime_error("Wallet is not encrypted");
    rewriteEncryptionPolicy(oldPassphrase, newPassphrase, true);
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
            const char* sql = "SELECT address FROM addresses WHERE account = 0 AND idx = 0 AND change = 0 LIMIT 1";
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (recovery_seeds_) throw std::logic_error("Wallet recovery key is pinned");
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

std::optional<std::array<uint8_t, 32>> WalletManager::loadInitialPqMaster(
    const std::vector<uint8_t>& seed) {
    if (!db_ || sqlite3_get_autocommit(db_) != 0)
        throw std::logic_error("Initial owner requires the unlock transaction");
    IssuedStatement owner(db_, "SELECT value FROM settings WHERE key=?");
    owner.Text(1, kInitialOwnerSetting);
    const int rc = sqlite3_step(owner.value.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    IssuanceCheck(db_, rc, SQLITE_ROW);
    if (sqlite3_column_type(owner.value.get(), 0) != SQLITE_TEXT)
        throw std::runtime_error("Initial owner type invalid");
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(owner.value.get(), 0));
    const int size = sqlite3_column_bytes(owner.value.get(), 0);
    if (!text || (size != 196 && size != 260))
        throw std::runtime_error("Initial owner envelope invalid");
    std::vector<uint8_t> bytes;
    if (!util::unhex(std::string(text, size), bytes))
        throw std::runtime_error("Initial owner encoding invalid");
    owner.Done();
    struct Secret { std::string value; ~Secret() { secureClearString(value); } } key{InitialOwnerKey(seed)}, plain;
    plain.value = decryptData(std::string(bytes.begin(), bytes.end()), key.value);
    if ((plain.value.size() != 70 && plain.value.size() != 102) || plain.value.substr(0,5) != "DNI01" ||
        (plain.value[5] != 0 && plain.value[5] != 1) ||
        plain.value.size() != (plain.value[5] == 1 ? 102u : 70u))
        throw std::runtime_error("Initial owner payload invalid");
    IssuedStatement id(db_, "SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
    IssuanceCheck(db_, sqlite3_step(id.value.get()), SQLITE_ROW);
    if (sqlite3_column_type(id.value.get(),0) != SQLITE_BLOB || sqlite3_column_bytes(id.value.get(),0) != 32)
        throw std::runtime_error("Initial owner identity missing");
    const auto* identity = static_cast<const uint8_t*>(sqlite3_column_blob(id.value.get(),0));
    if (!identity || plain.value.substr(6,64) != util::hex(std::vector<uint8_t>(identity,identity+32)))
        throw std::runtime_error("Initial owner identity mismatch");
    id.Done();
    std::optional<std::array<uint8_t,32>> result;
    if (plain.value[5] == 1) {
        result.emplace(); std::memcpy(result->data(), plain.value.data()+70,32);
    }
    return result;
}

void WalletManager::unlockWallet(const std::string& passphrase, int timeoutSeconds) {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (recovery_seeds_) throw std::logic_error("Wallet recovery key is pinned");
    if (!wallet_encrypted_) throw std::runtime_error("Wallet is not encrypted");
    auto lease = AcquireDatabaseLease();
    if (!lease || !db_ || current_wallet_id_ < 0)
        throw std::runtime_error("Wallet unlock owner unavailable");
    IssuedAddressTransaction transaction(db_);
    struct Secret {
        std::string value;
        ~Secret() { secureClearString(value); }
    } key, pq;
    struct Seed {
        std::vector<uint8_t> value;
        ~Seed() { secureClearBytes(value); }
    } seed;
    const auto setting = [&](const char* name) -> std::optional<std::string> {
        IssuedStatement q(db_, "SELECT value FROM settings WHERE key=?");
        q.Text(1, name);
        const int rc = sqlite3_step(q.value.get());
        if (rc == SQLITE_DONE) return std::nullopt;
        IssuanceCheck(db_, rc, SQLITE_ROW);
        if (sqlite3_column_type(q.value.get(), 0) != SQLITE_TEXT)
            throw std::runtime_error("Invalid unlock setting type");
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(q.value.get(), 0));
        const int size = sqlite3_column_bytes(q.value.get(), 0);
        if (!text || size < 0) throw std::runtime_error("Invalid unlock setting value");
        std::string value(text, size);
        q.Done();
        return value;
    };
    if (setting("wallet_encrypted") != std::optional<std::string>("1"))
        throw std::runtime_error("Wallet encryption policy mismatch");
    {
        IssuedStatement q(db_, "SELECT encrypted FROM encryption_metadata WHERE id=1");
        IssuanceCheck(db_, sqlite3_step(q.value.get()), SQLITE_ROW);
        if (sqlite3_column_type(q.value.get(),0) != SQLITE_INTEGER ||
            sqlite3_column_int64(q.value.get(),0) != 1)
            throw std::runtime_error("Wallet encryption metadata mismatch");
        q.Done();
    }
    const auto salt = setting("wallet_salt"), verify = setting("wallet_verify_hash");
    std::vector<uint8_t> salt_bytes, expected;
    if (!salt || !verify || !util::unhex(*salt, salt_bytes) || salt_bytes.size() != 32 ||
        !util::unhex(*verify, expected) || expected.size() != 32)
        throw std::runtime_error("Wallet unlock credentials missing");
    const std::string binary_salt(reinterpret_cast<const char*>(salt_bytes.data()), salt_bytes.size());
    key.value = deriveKey(passphrase, binary_salt);
    const auto verified = [&] {
        std::array<uint8_t,32> hash{};
        ::sha256(reinterpret_cast<const uint8_t*>(key.value.data()), key.value.size(), hash.data());
        const bool result = CRYPTO_memcmp(hash.data(), expected.data(), hash.size()) == 0;
        OPENSSL_cleanse(hash.data(), hash.size());
        return result;
    };
    bool legacy = false;
    if (!verified()) {
        secureClearString(key.value);
        key.value = deriveKeyLegacy(passphrase, binary_salt);
        if (!verified()) throw std::runtime_error("Invalid passphrase");
        legacy = true;
    }
    auto loaded = loadMasterSeed(passphrase);
    if (!loaded || loaded->size() != 64)
        throw std::runtime_error("Required wallet seed unavailable");
    seed.value = std::move(*loaded);
    if ((!master_seed_.empty() && !ConstantTimeEqual(seed.value, master_seed_)) ||
        (!encryption_key_.empty() && key.value != encryption_key_))
        throw std::runtime_error("Live wallet unlock owner mismatch");

    AuthenticateHdInventory(db_,seed.value);

    // Authenticate every present predecessor-format import in the same
    // snapshot before publishing any unlock authority. These records used the
    // historical SHA256(internal_xonly || 0x00) tweak and MAIN address domain;
    // substituting modern TapTweak or the current network changes ownership.
    {
        IssuedStatement inventory(db_, "SELECT address,private_key_enc FROM imported_keys ORDER BY address");
        int rc;
        while ((rc=sqlite3_step(inventory.value.get()))==SQLITE_ROW) {
            const auto text=[&](int col) {
                if (sqlite3_column_type(inventory.value.get(),col)!=SQLITE_TEXT)
                    throw std::runtime_error("Legacy import field type invalid");
                const auto* bytes=static_cast<const char*>(sqlite3_column_blob(inventory.value.get(),col));
                const int size=sqlite3_column_bytes(inventory.value.get(),col);
                if (!bytes || size<=0) throw std::runtime_error("Legacy import field missing");
                return std::string(bytes,size);
            };
            const auto address=text(0);
            Secret stored,plain;
            stored.value=text(1);
            if (stored.value.size()!=60 && stored.value.size()!=92)
                throw std::runtime_error("Legacy import ciphertext length invalid");
            plain.value=decryptData(stored.value,key.value);
            Seed decoded;
            if (plain.value.size()==32) decoded.value.assign(plain.value.begin(),plain.value.end());
            else if (plain.value.size()!=64 || !util::unhex(plain.value,decoded.value) || decoded.value.size()!=32)
                throw std::runtime_error("Legacy import plaintext invalid");
            struct Scalar {
                std::array<uint8_t,32> value{};
                ~Scalar(){OPENSSL_cleanse(value.data(),value.size());}
            } scalar;
            std::copy(decoded.value.begin(),decoded.value.end(),scalar.value.begin());
            std::array<uint8_t,32> internal{},output{},tweak{};
            int parity=0;
            if (!TaprootKeys::DeriveXOnlyPubkey(scalar.value,internal,parity))
                throw std::runtime_error("Legacy import scalar invalid");
            std::array<uint8_t,33> input{};
            std::copy(internal.begin(),internal.end(),input.begin());
            ::SHA256(input.data(),input.size(),tweak.data());
            std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> context(
                secp256k1_context_create(SECP256K1_CONTEXT_VERIFY),secp256k1_context_destroy);
            secp256k1_xonly_pubkey point{},result{};secp256k1_pubkey tweaked{};
            if (!context || !secp256k1_xonly_pubkey_parse(context.get(),&point,internal.data()) ||
                !secp256k1_xonly_pubkey_tweak_add(context.get(),&tweaked,&point,tweak.data()) ||
                !secp256k1_xonly_pubkey_from_pubkey(context.get(),&result,nullptr,&tweaked) ||
                !secp256k1_xonly_pubkey_serialize(context.get(),output.data(),&result))
                throw std::runtime_error("Legacy import public binding invalid");
            const auto expected=AddressCodec::encodeP2TR(Network::MAIN,std::vector<uint8_t>(output.begin(),output.end()));
            if (expected.empty() || address!=expected)
                throw std::runtime_error("Legacy import historical address mismatch");
        }
        IssuanceCheck(db_,rc,SQLITE_DONE);
    }

    // Authenticate present modern imported owners and their recorded script
    // bindings under this unlock snapshot. Absence is not a completeness proof.
    {
        IssuedStatement table(db_, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='taproot_keys'");
        const int found=sqlite3_step(table.value.get());
        if (found==SQLITE_ROW) {
            table.Done();
            IssuedStatement inventory(db_, "SELECT address,internal_privkey,internal_pubkey,output_pubkey,is_privkey_encrypted FROM taproot_keys ORDER BY address");
            const auto text=[](sqlite3_stmt* q,int col) {
                if(sqlite3_column_type(q,col)!=SQLITE_TEXT)
                    throw std::runtime_error("Imported text field invalid");
                const auto* p=static_cast<const char*>(sqlite3_column_blob(q,col));
                const int n=sqlite3_column_bytes(q,col);
                if(!p || n<=0)throw std::runtime_error("Imported text field missing");
                return std::string(p,n);
            };
            const auto integer=[](sqlite3_stmt* q,int col,int expected) {
                return sqlite3_column_type(q,col)==SQLITE_INTEGER &&
                       sqlite3_column_int64(q,col)==expected;
            };
            const auto blob=[](sqlite3_stmt* q,int col,const uint8_t* expected,int n) {
                return sqlite3_column_type(q,col)==SQLITE_BLOB &&
                       sqlite3_column_bytes(q,col)==n && sqlite3_column_blob(q,col) &&
                       CRYPTO_memcmp(sqlite3_column_blob(q,col),expected,n)==0;
            };
            const bool wallet_column=IssuanceWalletColumn(db_,"addresses");
            int rc;
            while((rc=sqlite3_step(inventory.value.get()))==SQLITE_ROW) {
                auto* q=inventory.value.get();
                const auto address=text(q,0);
                if(!integer(q,4,1) || sqlite3_column_type(q,1)!=SQLITE_BLOB ||
                   sqlite3_column_bytes(q,1)!=60 || !sqlite3_column_blob(q,1))
                    throw std::runtime_error("Imported unlock policy invalid");
                Secret stored,plain;
                stored.value.assign(static_cast<const char*>(sqlite3_column_blob(q,1)),60);
                plain.value=decryptData(stored.value,key.value);
                if(plain.value.size()!=32)throw std::runtime_error("Imported scalar length invalid");
                struct Scalar {
                    std::array<uint8_t,32> value{};
                    ~Scalar(){OPENSSL_cleanse(value.data(),value.size());}
                } scalar;
                std::copy(plain.value.begin(),plain.value.end(),scalar.value.begin());
                std::array<uint8_t,32> internal{},output{};int parity=0;
                if(!TaprootKeys::DeriveXOnlyPubkey(scalar.value,internal,parity) ||
                   !TaprootKeys::ComputeTweakedPubkey(internal,output) ||
                   !blob(q,2,internal.data(),32) || !blob(q,3,output.data(),32))
                    throw std::runtime_error("Imported unlock public binding mismatch");
                const auto& network=Params().name;
                if(address!=TaprootKeys::CreateTaprootAddress(output,
                   network=="regtest"?"rdin":network=="testnet"?"tdin":"din"))
                    throw std::runtime_error("Imported unlock address mismatch");
                std::vector<uint8_t> script{0x51,0x20};
                script.insert(script.end(),output.begin(),output.end());
                const auto path="tr("+util::hex(std::vector<uint8_t>(internal.begin(),internal.end())).substr(0,8)+"...)";
                const std::string sql=R"(SELECT m.internal_pubkey,m.derivation_path,w.path,w.is_change,
                    a.account,a.change,a.type,a.script_pubkey,)"+
                    std::string(wallet_column?"a.wallet_id":"1")+R"( FROM taproot_key_mapping m
                    JOIN watch_scripts w ON w.script_pubkey=?
                    JOIN addresses a ON a.address=?
                    WHERE m.output_pubkey=?)";
                IssuedStatement binding(db_,sql.c_str());
                binding.Blob(1,script.data(),int(script.size()));
                binding.Text(2,address);binding.Blob(3,output.data(),32);
                IssuanceCheck(db_,sqlite3_step(binding.value.get()),SQLITE_ROW);
                auto* row=binding.value.get();
                if(!blob(row,0,internal.data(),32) || text(row,1)!=path || text(row,2)!=path ||
                   !integer(row,3,0) || !integer(row,4,-1) || !integer(row,5,0) ||
                   text(row,6)!="p2tr" || text(row,7)!=util::hex(script) || !integer(row,8,1))
                    throw std::runtime_error("Imported unlock script binding mismatch");
                binding.Done();
            }
            IssuanceCheck(db_,rc,SQLITE_DONE);
        } else IssuanceCheck(db_,found,SQLITE_DONE);
    }

    // Read every known master owner in the same transaction. A malformed
    // present owner is an error even when another wrapper can be decrypted.
    auto initial = loadInitialPqMaster(seed.value);
    struct ClearInitial {
        std::optional<std::array<uint8_t,32>>& value;
        ~ClearInitial() { if (value) OPENSSL_cleanse(value->data(), value->size()); }
    } clear_initial{initial};
    const auto wrapped = setting("v7_pq_master_key_encrypted");
    if (wrapped) {
        std::vector<uint8_t> bytes;
        if (!util::unhex(*wrapped, bytes) || bytes.size() != 60)
            throw std::runtime_error("PQ master wrapper invalid");
        pq.value = decryptData(std::string(bytes.begin(),bytes.end()),key.value);
        if (pq.value.size() != 32) throw std::runtime_error("PQ master invalid");
        if (initial && CRYPTO_memcmp(pq.value.data(), initial->data(),32) != 0)
            throw std::runtime_error("PQ master owners disagree");
    } else if (initial) {
        pq.value.assign(reinterpret_cast<const char*>(initial->data()),initial->size());
    }
    if (pq_master_key_loaded_ && (pq.value.size() != 32 ||
        CRYPTO_memcmp(pq.value.data(),pq_master_key_.data(),32) != 0))
        throw std::runtime_error("Live PQ master mismatch");

    // The separate PQ store contributes one immutable statement snapshot.
    // Keep the actual main-wallet lifecycle owner and staged master throughout
    // authentication. This does not make the two databases one atomic owner.
    {
        const auto path=GetV7P2MRStorePath();
        if(path.empty())throw std::runtime_error("PQ inventory path unavailable");
        std::error_code error;
        const auto status=std::filesystem::symlink_status(path,error);
        if(error && error!=std::errc::no_such_file_or_directory)
            throw std::runtime_error("PQ inventory storage unavailable");
        if(std::filesystem::exists(status)) {
            wallet::V7P2MRStore store;
            if(store.OpenExistingReadOnly(path)!=wallet::V7P2MRStore::OpenResult::Ok)
                throw std::runtime_error("PQ inventory storage invalid");
            const auto captured=store.CaptureKeysByWallet(current_wallet_id_);
            if(!captured.empty() && pq.value.size()!=32)
                throw std::runtime_error("PQ inventory master unavailable");
            struct Master {
                wallet::AeadKey value{};
                ~Master(){OPENSSL_cleanse(value.data(),value.size());}
            } master;
            if(pq.value.size()==32)std::memcpy(master.value.data(),pq.value.data(),32);
            for(const auto& record:captured) {
                wallet::SecureSeed plaintext;
                const auto& encrypted=record.encrypted_seed;
                if(wallet::OpenSeedSecure(encrypted.ciphertext,encrypted.nonce,encrypted.tag,
                       master.value,&plaintext)!=wallet::AeadOpenResult::Ok)
                    throw std::runtime_error("PQ inventory authentication failed");
                wallet::SecureKeypair pair(consensus::pq::ml_dsa_65::KeygenFromSeed(plaintext.bytes()));
                std::array<uint8_t,32> root{};
                const uint8_t scheme=consensus::pq::SCHEME_ID_ML_DSA_65;
                crypto::CSHA256().Write(&scheme,1).Write(pair.pubkey().data(),pair.pubkey().size()).Finalize(root.data());
                const auto& row=record.metadata;
                const auto decoded=wallet::DecodeP2MRAddress(row.address);
                if(pair.pubkey()!=row.pubkey || root!=row.merkle_root ||
                   !decoded || decoded->merkle_root!=root)
                    throw std::runtime_error("PQ inventory key binding mismatch");
                const auto script=wallet::BuildP2MRScriptPubKey(root);
                IssuedStatement watched(db_,"SELECT path,is_change FROM watch_scripts WHERE script_pubkey=?");
                watched.Blob(1,script.data(),int(script.size()));
                IssuanceCheck(db_,sqlite3_step(watched.value.get()),SQLITE_ROW);
                auto* q=watched.value.get();
                if(sqlite3_column_type(q,0)!=SQLITE_TEXT ||
                   sqlite3_column_bytes(q,0)!=int(row.derivation_path.size()) ||
                   !sqlite3_column_blob(q,0) ||
                   std::memcmp(sqlite3_column_blob(q,0),row.derivation_path.data(),row.derivation_path.size())!=0 ||
                   sqlite3_column_type(q,1)!=SQLITE_INTEGER || sqlite3_column_int64(q,1)!=0)
                    throw std::runtime_error("PQ inventory recognition binding mismatch");
                watched.Done();
            }
        }
        // Missing storage is not a complete inventory or permission to create
        // any historical master. Watch-only and deleted owners remain unknown.
    }

    std::vector<ShieldedIncomingViewingKey> incoming, outgoing;
    std::vector<ShieldedRecipientViewingAuthority> recipients;
    struct ClearViews {
        std::vector<ShieldedIncomingViewingKey>& incoming;
        std::vector<ShieldedIncomingViewingKey>& outgoing;
        std::vector<ShieldedRecipientViewingAuthority>& recipients;
        ~ClearViews() {
            if (!incoming.empty()) OPENSSL_cleanse(incoming.data(),incoming.size()*sizeof(incoming[0]));
            if (!outgoing.empty()) OPENSSL_cleanse(outgoing.data(),outgoing.size()*sizeof(outgoing[0]));
            if (!recipients.empty()) OPENSSL_cleanse(recipients.data(),recipients.size()*sizeof(recipients[0]));
        }
    } clear_views{incoming,outgoing,recipients};
    for (uint32_t account=0; account<4; ++account) {
        auto keys=wallet::shielded::DeriveShieldedAccount(seed.value.data(),seed.value.size(),account);
        ScopedShieldedAccountKeys guard(keys);
        incoming.push_back(keys.ivk); outgoing.push_back(keys.ovk);
        recipients.push_back({keys.ivk,keys.ak,keys.nvk});
    }
    if (!wrapped && initial) {
        const auto encrypted=encryptData(pq.value,key.value);
        IssuedStatement q(db_, "INSERT INTO settings(key,value,updated_at) VALUES('v7_pq_master_key_encrypted',?,strftime('%s','now'))");
        q.Text(1,util::hex(std::vector<uint8_t>(encrypted.begin(),encrypted.end())));
        q.Done(true);
    }
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (legacy) WLOG_WARN("Wallet uses legacy HMAC-SHA512 KDF; change password to upgrade");
    (void)ReadPendingPaymentsOwned(seed.value);
    transaction.Commit();
    // All fallible validation and persistence precede publication. Swaps leave
    // prior plaintext in scoped buffers for cleansing without changing identity.
    encryption_key_.swap(key.value);
    master_seed_.swap(seed.value);
    OPENSSL_cleanse(pq_master_key_.data(),pq_master_key_.size());
    pq_master_key_loaded_=!pq.value.empty();
    if (pq_master_key_loaded_) std::memcpy(pq_master_key_.data(),pq.value.data(),32);
    shielded_incoming_viewing_keys_.swap(incoming);
    shielded_outgoing_viewing_keys_.swap(outgoing);
    shielded_recipient_viewing_authorities_.swap(recipients);
    unlock_timeout_=timeoutSeconds>0 ? timeoutSeconds : 0;
    unlock_time_=timeoutSeconds>0 ? now : 0;
    wallet_locked_=false;
}

// ═══════════════════════════════════════════════════════════════
// V7 post-quantum wallet accessors (spec V7_WALLET_SCHEMA.md §5b)
// ═══════════════════════════════════════════════════════════════

std::optional<std::array<uint8_t, 32>> WalletManager::GetV7PqMasterKey() const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
            ScopedShieldedAccountKeys keys_guard(keys);
            ivks.push_back(keys.ivk);
        }
    } catch (...) {
        ivks.clear();
    }
    return ivks;
}

std::vector<WalletManager::ShieldedOutgoingViewingKey>
WalletManager::GetShieldedOutgoingViewingKeys() const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (!shielded_outgoing_viewing_keys_.empty()) {
        return shielded_outgoing_viewing_keys_;
    }
    if (master_seed_.size() != 64) return {};

    std::vector<ShieldedOutgoingViewingKey> ovks;
    try {
        constexpr uint32_t kShieldedScanAccounts = 4;
        for (uint32_t acct = 0; acct < kShieldedScanAccounts; ++acct) {
            auto keys = wallet::shielded::DeriveShieldedAccount(
                master_seed_.data(), master_seed_.size(), acct);
            ScopedShieldedAccountKeys keys_guard(keys);
            ovks.push_back(keys.ovk);
        }
    } catch (...) {
        for (auto& ovk : ovks) OPENSSL_cleanse(ovk.data(), ovk.size());
        ovks.clear();
    }
    return ovks;
}

std::vector<WalletManager::ShieldedRecipientViewingAuthority>
WalletManager::GetShieldedRecipientViewingAuthorities() const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (!shielded_recipient_viewing_authorities_.empty()) {
        return shielded_recipient_viewing_authorities_;
    }
    if (master_seed_.size() != 64) return {};

    std::vector<ShieldedRecipientViewingAuthority> authorities;
    try {
        constexpr uint32_t kShieldedScanAccounts = 4;
        for (uint32_t acct = 0; acct < kShieldedScanAccounts; ++acct) {
            auto keys = wallet::shielded::DeriveShieldedAccount(
                master_seed_.data(), master_seed_.size(), acct);
            ScopedShieldedAccountKeys keys_guard(keys);
            authorities.push_back({keys.ivk, keys.ak, keys.nvk});
        }
    } catch (...) {
        for (auto& authority : authorities) {
            OPENSSL_cleanse(&authority, sizeof(authority));
        }
        authorities.clear();
    }
    return authorities;
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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

        struct Bytes {
            std::array<uint8_t,32> value{};
            ~Bytes() { OPENSSL_cleanse(value.data(),value.size()); }
        } priv{deriver.getPrivateKey()}, chain{deriver.getChainCode()};
        if (priv.value.size()!=32 || chain.value.size()!=32) return std::nullopt;
        V7Bip32Material out{};
        struct ClearMaterial { V7Bip32Material& value; ~ClearMaterial() {
            OPENSSL_cleanse(value.private_key.data(),value.private_key.size());
            OPENSSL_cleanse(value.chain_code.data(),value.chain_code.size());
        }} clear_out{out};
        std::memcpy(out.private_key.data(), priv.value.data(), out.private_key.size());
        std::memcpy(out.chain_code.data(), chain.value.data(), out.chain_code.size());
        return out;
    } catch (const std::exception& e) {
        WLOG_WARN(std::string("DeriveV7Bip32Material failed: ") + e.what());
        return std::nullopt;
    }
}

bool WalletManager::isWalletEncrypted() const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    return getSetting("wallet_encrypted") == "1";
}

bool WalletManager::isWalletLocked() const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (unlock_timeout_ > 0 && unlock_time_ > 0) {
        int64_t currentTime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        if (currentTime - unlock_time_ >= unlock_timeout_) {
            if (recovery_seeds_) throw std::logic_error("Wallet recovery key is pinned");
            wallet_locked_ = true;
            secureClearString(encryption_key_);
            clearPrivateKeyCache();
            secureClearBytes(master_seed_);
            OPENSSL_cleanse(pq_master_key_.data(), pq_master_key_.size());
            pq_master_key_loaded_ = false;
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
    auto lease=AcquireDatabaseLease();
    std::string outpoint = txid + ":" + std::to_string(vout);
    locked_utxos_.insert(outpoint);
    return true;
}

bool WalletManager::unlockUTXO(const std::string& txid, uint32_t vout) {
    auto lease=AcquireDatabaseLease();
    for(const auto& p:getPendingPayments())for(const auto& in:p.inputs)
        if(in.txid==txid && in.vout==vout)return false;
    std::string outpoint = txid + ":" + std::to_string(vout);
    auto it = locked_utxos_.find(outpoint);
    if (it != locked_utxos_.end()) {
        locked_utxos_.erase(it);
        return true;
    }
    return false;  // Was not locked
}

bool WalletManager::isUTXOLocked(const std::string& txid, uint32_t vout) const {
    auto lease=const_cast<WalletManager&>(*this).AcquireDatabaseLease();
    const auto records=getPendingPayments(); // Authenticate even if a manual lock matches.
    for(const auto& p:records)for(const auto& in:p.inputs)
        if(in.txid==txid && in.vout==vout)return true;
    return locked_utxos_.count(txid+":"+std::to_string(vout))!=0;
}

std::vector<std::string> WalletManager::getLockedUTXOs() const {
    auto lease=const_cast<WalletManager&>(*this).AcquireDatabaseLease();
    auto result=locked_utxos_;
    for(const auto& p:getPendingPayments())for(const auto& in:p.inputs)
        result.insert(in.txid+":"+std::to_string(in.vout));
    return {result.begin(),result.end()};
}

size_t WalletManager::unlockAllUTXOs() {
    auto lease=AcquireDatabaseLease();
    (void)getPendingPayments(); // Refuse unavailable ownership; never release payment reservations.
    const auto count=locked_utxos_.size();locked_utxos_.clear();return count;
}

WalletManager::BalanceSummary WalletManager::getBalanceSummary(const std::string& expected_wallet) const {
    auto& self = const_cast<WalletManager&>(*this);
    auto lease = self.AcquireDatabaseLease();
    if (!db_ || current_wallet_id_ < 0 ||
        (!expected_wallet.empty() && lease->WalletName() != expected_wallet))
        throw std::runtime_error("Balance wallet ownership unavailable");
    IssuedAddressTransaction transaction(db_);
    BalanceSummary result;
    result.wallet_name = lease->WalletName();
    auto locks = locked_utxos_;
    std::unique_ptr<RecoverySeed> pin;
    const bool installed = PaymentColumn(db_);
    self.checkUnlockTimeout();
    if (installed && wallet_locked_) {
        result.reservations = ReservationStatus::UnlockRequired;
    } else if (installed) {
        pin = lease->CopyRecoverySeed(lease->Session());
        for (const auto& payment : ReadPendingPaymentsOwned(pin->Bytes()))
            for (const auto& input : payment.inputs)
                locks.insert(input.txid + ":" + std::to_string(input.vout));
        result.reservations = ReservationStatus::Authenticated;
    }
    // Capture the wallet's observed height without acquiring selected-chain locks.
    // This is not a certificate that the wallet has caught up to the chain.
    const int64_t tip = getBlockchainHeight();
    uint64_t confirmed = 0, unconfirmed = 0, immature = 0, locked = 0, locked_confirmed = 0;
    std::map<std::string, uint64_t> available_by_script;
    IssuedStatement query(db_, "SELECT txid,vout,amount,height,is_coinbase,is_spent,script_pubkey "
                               "FROM utxos WHERE wallet_id=? ORDER BY txid,vout");
    query.Int(1, current_wallet_id_);
    int rc;
    while ((rc = sqlite3_step(query.value.get())) == SQLITE_ROW) {
        auto* q = query.value.get();
        for (int col : {1, 2, 3, 4, 5})
            if (sqlite3_column_type(q, col) != SQLITE_INTEGER)
                throw std::runtime_error("Balance coin integer malformed");
        const auto index = sqlite3_column_int64(q, 1), amount = sqlite3_column_int64(q, 2);
        const auto height = sqlite3_column_int64(q, 3), coinbase = sqlite3_column_int64(q, 4);
        const auto spent = sqlite3_column_int64(q, 5);
        if (index < 0 || index > UINT32_MAX || amount < 0 || height < -1 || height > UINT32_MAX ||
            (coinbase != 0 && coinbase != 1) || (spent != 0 && spent != 1))
            throw std::runtime_error("Balance coin range malformed");
        const auto text = [&](int col) {
            if (sqlite3_column_type(q, col) != SQLITE_TEXT)
                throw std::runtime_error("Balance coin text malformed");
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(q, col));
            const int size = sqlite3_column_bytes(q, col);
            if (!bytes || size <= 0) throw std::runtime_error("Balance coin text missing");
            return std::string(bytes, size);
        };
        const auto txid = text(0), script = text(6);
        std::vector<uint8_t> decoded_txid, decoded_script;
        if (txid.size() != 64 || !util::unhex(txid, decoded_txid) ||
            txid != util::hex(decoded_txid) ||
            !util::unhex(script, decoded_script) || decoded_script.empty())
            throw std::runtime_error("Balance coin encoding malformed");
        if (spent) continue;
        if (result.balance.utxo_count == INT_MAX) throw std::runtime_error("Balance coin count exceeded");
        ++result.balance.utxo_count;
        const uint64_t value = static_cast<uint64_t>(amount);
        const int64_t confirmations = height > 0 ? tip - height + 1 : 0;
        const bool eligible = confirmations >= 1 && (!coinbase || confirmations >= 100);
        if (confirmations < 1) unconfirmed = PaymentSum(unconfirmed, value);
        else if (!eligible) {
            immature = PaymentSum(immature, value);
            ++result.balance.immature_utxo_count;
        } else {
            confirmed = PaymentSum(confirmed, value);
            if (decoded_script.size() == 34 && decoded_script[0] == 0x53 && decoded_script[1] == 0x20)
                result.pq_confirmed_una = PaymentSum(result.pq_confirmed_una, value);
        }
        auto& script_available = available_by_script[util::hex(decoded_script)];
        const bool reserved = locks.count(txid + ":" + std::to_string(index)) != 0;
        if (eligible && !reserved) script_available = PaymentSum(script_available, value);
        if (reserved) {
            locked = PaymentSum(locked, value);
            if (eligible) locked_confirmed = PaymentSum(locked_confirmed, value);
        }
    }
    IssuanceCheck(db_, rc, SQLITE_DONE);
    const auto total = PaymentSum(PaymentSum(confirmed, unconfirmed), immature);
    result.balance.confirmed = static_cast<double>(confirmed) / 1e8;
    result.balance.unconfirmed = static_cast<double>(unconfirmed) / 1e8;
    result.balance.immature = static_cast<double>(immature) / 1e8;
    result.balance.total = static_cast<double>(total) / 1e8;
    // Consumers must use the optional value; never turn unknown reservations into zero locks.
    if (result.reservations != ReservationStatus::UnlockRequired) {
        for (const auto& [script, value] : available_by_script)
            result.available_by_script.emplace(script, static_cast<double>(value) / 1e8);
        result.locked = static_cast<double>(locked) / 1e8;
        result.available_confirmed = static_cast<double>(confirmed - locked_confirmed) / 1e8;
        result.unavailable_confirmed_and_immature = static_cast<double>(PaymentSum(immature, locked_confirmed)) / 1e8;
    }
    transaction.Commit();
    return result;
}

double WalletManager::getLockedBalance() const {
    auto& self=const_cast<WalletManager&>(*this);auto lease=self.AcquireDatabaseLease();
    if(!db_)return 0.0;
    IssuedAddressTransaction transaction(db_);auto locks=locked_utxos_;
    if(PaymentColumn(db_)) {
        auto pin=lease->CopyRecoverySeed(lease->Session());
        for(const auto& p:ReadPendingPaymentsOwned(pin->Bytes()))for(const auto& in:p.inputs)
            locks.insert(in.txid+":"+std::to_string(in.vout));
    }
    uint64_t amount=0;
    for(const auto& outpoint:locks) {
        const auto colon=outpoint.find(':');
        if(colon==std::string::npos)throw std::runtime_error("Locked outpoint malformed");
        const auto index=std::stoull(outpoint.substr(colon+1));
        if(index>UINT32_MAX)throw std::runtime_error("Locked outpoint range invalid");
        IssuedStatement q(db_,"SELECT amount,is_spent FROM utxos WHERE wallet_id=? AND txid=? AND vout=?");
        q.Int(1,current_wallet_id_);q.Text(2,outpoint.substr(0,colon));q.Int(3,int64_t(index));
        const int rc=sqlite3_step(q.value.get());if(rc==SQLITE_DONE)continue;
        IssuanceCheck(db_,rc,SQLITE_ROW);
        if(sqlite3_column_type(q.value.get(),0)!=SQLITE_INTEGER || sqlite3_column_int64(q.value.get(),0)<0 ||
           sqlite3_column_type(q.value.get(),1)!=SQLITE_INTEGER ||
           (sqlite3_column_int64(q.value.get(),1)!=0 && sqlite3_column_int64(q.value.get(),1)!=1))
            throw std::runtime_error("Locked coin malformed");
        if(sqlite3_column_int64(q.value.get(),1)==0)amount=PaymentSum(amount,uint64_t(sqlite3_column_int64(q.value.get(),0)));
        q.Done();
    }
    transaction.Commit();return static_cast<double>(amount)/1e8;
}

// Phase 35.4: Transaction Abandonment
bool WalletManager::abandonTransaction(const std::string& txid) {
    auto lease=AcquireDatabaseLease();
    for(const auto& p:getPendingPayments())if(p.txid==txid)return false;
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
    auto lease=const_cast<WalletManager&>(*this).AcquireDatabaseLease();
    AbandonmentInfo info;
    info.success = false;
    info.inputs_returned = 0;
    info.amount_returned = 0.0;

    if (!db_) {
        info.error = "Wallet not loaded";
        return info;
    }

    for(const auto& p:getPendingPayments())if(p.txid==txid) {
        info.error="Payment retains durable input reservations; reconciliation is required";
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

bool WalletManager::confirmTransaction(const std::string& txid, uint32_t height, std::string* error) {
    if (error) error->clear();
    if (!db_ || current_wallet_id_ == -1 || txid.empty() || height == 0) {
        if (error) *error = "Wallet confirmation requires a selected wallet and valid identity";
        return false;
    }

    if (!columnExists(db_, "transactions", "height") ||
        !columnExists(db_, "transactions", "confirmations")) {
        if (error) *error = "transactions table lacks height/confirmations columns";
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
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }

    int bind_index = 1;
    int bound = sqlite3_bind_int(stmt, bind_index++, static_cast<int>(height));
    if (bound == SQLITE_OK) bound = sqlite3_bind_int(stmt, bind_index++, confirmations);
    if (bound == SQLITE_OK && has_wallet_id)
        bound = sqlite3_bind_int(stmt, bind_index++, current_wallet_id_);
    if (bound == SQLITE_OK)
        bound = sqlite3_bind_text(stmt, bind_index++, txid.c_str(), -1, SQLITE_STATIC);
    if (bound != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return false;
    }

    const int result = sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (result != SQLITE_DONE) {
        if (error) *error = sqlite3_errmsg(db_);
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

namespace {
// Caller owns the surrounding checked transaction. Chain rollback changes
// confirmation metadata; an existing outgoing intent remains local history.
void UnconfirmOutgoingHistoryAtHeight(sqlite3* db, int wallet_id, uint32_t height, bool scoped) {
    if (sqlite3_get_autocommit(db)) throw std::logic_error("History rewind requires its transaction owner");
    IssuedStatement statement(db, scoped
        ? "UPDATE transactions SET height=0,confirmations=0 WHERE wallet_id=? AND height=? AND category='send' AND amount<0"
        : "UPDATE transactions SET height=0,confirmations=0 WHERE height=? AND category='send' AND amount<0");
    int parameter=1;
    if(scoped) statement.Int(parameter++,wallet_id);
    statement.Int(parameter,height);statement.Done();
}
}

bool WalletManager::removeTransactionsAtHeight(uint32_t height) {
    const auto lease=AcquireDatabaseLease();
    if(!db_ || current_wallet_id_==-1) return false;
    if(height==0 || height>static_cast<uint32_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Wallet history rewind height is out of range");
    IssuedAddressTransaction transaction(db_);
    const bool scoped=IssuanceWalletColumn(db_,"transactions");
    UnconfirmOutgoingHistoryAtHeight(db_,current_wallet_id_,height,scoped);
    IssuedStatement statement(db_,scoped
        ? "DELETE FROM transactions WHERE wallet_id=? AND height=?"
        : "DELETE FROM transactions WHERE height=?");
    int parameter=1;if(scoped)statement.Int(parameter++,current_wallet_id_);
    statement.Int(parameter,height);statement.Done();transaction.Commit();
    return true;
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if(!db_ || !sqlite3_get_autocommit(db_))return "";
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
        IssuedAddressTransaction issuance(db_);
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

        const uint32_t purpose=(effective_address_type=="taproot")?86:84;
        const std::string derivation_path="m/"+std::to_string(purpose)+"'/"+
            std::to_string(dinero::consensus::DINERO_COIN_TYPE)+"'/0'/0/"+std::to_string(next_index);
        PersistIssuedAddress(db_,0,next_index,address,label,
            effective_address_type=="taproot"?"p2tr":"p2wpkh",script_pubkey,script_bytes,derivation_path,
            key_id,internal_key_id,output_key_id);
        issuance.Commit();
        // Live recognition follows the durable tuple. If publication throws,
        // the issued address remains retained for ordinary reopen/recovery.

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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if(!db_ || !sqlite3_get_autocommit(db_))return "";
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
        IssuedAddressTransaction issuance(db_);
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

        const uint32_t purpose=(effective_address_type=="taproot")?86:84;
        const std::string derivation_path="m/"+std::to_string(purpose)+"'/"+
            std::to_string(dinero::consensus::DINERO_COIN_TYPE)+"'/0'/1/"+std::to_string(next_index);
        PersistIssuedAddress(db_,1,next_index,address,label,
            effective_address_type=="taproot"?"p2tr":"p2wpkh",script_pubkey,script_bytes,derivation_path,
            key_id,internal_key_id,output_key_id);
        issuance.Commit();
        // Live recognition follows the durable tuple. If publication throws,
        // the issued address remains retained for ordinary reopen/recovery.

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
    return RescanBlockchainImpl(start_height, gap_limit, chain_db, block_storage, nullptr, 0);
}

bool WalletManager::RescanBlockchainImpl(int start_height, int gap_limit,
                                        ChainDB* chain_db, BlockStorage* block_storage,
                                        const SelectedWalletHistory* prepared, uint64_t expected_session) {
    if (!chain_db && !prepared) return false;
    std::unique_ptr<DatabaseLease> database_lease;
    try { database_lease = AcquireDatabaseLease(); }
    catch (...) { return false; }
    if (!db_ || current_wallet_id_ == -1 || !sqlite3_get_autocommit(db_)) return false;
    if (prepared && (database_leases_ != 1 || !expected_session ||
        database_lease->Session() != expected_session || prepared->blocks_.empty() ||
        prepared->network_ != Params().network_id ||
        prepared->genesis_ != uint256::FromHexUnsafe(Params().genesis_hash))) return false;

    // The prepared path reads only owned immutable bodies. The legacy path
    // still reads mutable archival data. Neither grants a recovery receipt.
    auto checked = [&](int result, int expected) {
        if (result != expected)
            throw std::runtime_error(std::string("Block rescan SQL failed: ") + sqlite3_errmsg(db_));
    };
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    auto prepare = [&](const char* sql) {
        sqlite3_stmt* raw = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
        Statement statement(raw, sqlite3_finalize);
        checked(rc, SQLITE_OK);
        return statement;
    };
    auto watch_scripts = [&] {
        std::set<std::vector<uint8_t>> scripts;
        auto statement = prepare("SELECT script_pubkey FROM watch_scripts");
        int rc;
        while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto* bytes = static_cast<const uint8_t*>(sqlite3_column_blob(statement.get(), 0));
            const int size = sqlite3_column_bytes(statement.get(), 0);
            if (sqlite3_column_type(statement.get(), 0) != SQLITE_BLOB || !bytes || size <= 0)
                throw std::runtime_error("Block rescan watch script unavailable");
            scripts.emplace(bytes, bytes + size);
        }
        checked(rc, SQLITE_DONE);
        return scripts;
    };
    bool owned_transaction = false;
    std::unique_lock<std::mutex> height_lock(height_mu_, std::defer_lock);
    uint32_t tip_height = 0;
    try {
        if (prepared) {
            if (prepared->blocks_.size() > uint64_t(INT32_MAX) + 1 ||
                prepared->blocks_.back().GetHash() != prepared->tip_) return false;
            tip_height = static_cast<uint32_t>(prepared->blocks_.size() - 1);
        } else {
            const auto tip = chain_db->getTip();
            if (!tip.ok() || tip->height < 0) return false;
            tip_height = static_cast<uint32_t>(tip->height);
        }
        start_height = std::max(start_height, 0);
        if (static_cast<uint32_t>(start_height) > tip_height) return true;
        if (gap_limit < 0) return false;
        auto scripts = watch_scripts();
        // Preserve existing address discovery. Issued addresses are durable
        // independently of the scan and must not be rolled back with effects.
        if (scripts.size() < uint64_t(gap_limit) * 2 && !master_seed_.empty()) {
            for (int i = getNextAddressIndex(0, 0); i < gap_limit; ++i)
                if (getNewAddress("", "taproot").empty()) return false;
            for (int i = getNextAddressIndex(0, 1); i < gap_limit; ++i)
                if (getNewChangeAddress("", "taproot").empty()) return false;
            scripts = watch_scripts();
        }
        if (scripts.empty()) return true;
        exec(db_, "PRAGMA synchronous=FULL");
        {
            auto policy = prepare("PRAGMA synchronous");
            checked(sqlite3_step(policy.get()), SQLITE_ROW);
            if (sqlite3_column_int(policy.get(), 0) != 2)
                throw std::runtime_error("Block rescan durability unavailable");
            checked(sqlite3_step(policy.get()), SQLITE_DONE);
        }
        exec(db_, "BEGIN IMMEDIATE");
        owned_transaction = true;
        // Cleanup and every required SQL effect/progress update share this
        // transaction. Never adopt, commit or roll back a caller's transaction.
        {
            auto remove = prepare("DELETE FROM utxos WHERE wallet_id=? AND height>=?");
            checked(sqlite3_bind_int(remove.get(), 1, current_wallet_id_), SQLITE_OK);
            checked(sqlite3_bind_int(remove.get(), 2, start_height), SQLITE_OK);
            checked(sqlite3_step(remove.get()), SQLITE_DONE);
            auto restore = prepare("UPDATE utxos SET is_spent=0,spent_txid=NULL,spent_height=NULL WHERE wallet_id=? AND spent_height>=?");
            checked(sqlite3_bind_int(restore.get(), 1, current_wallet_id_), SQLITE_OK);
            checked(sqlite3_bind_int(restore.get(), 2, start_height), SQLITE_OK);
            checked(sqlite3_step(restore.get()), SQLITE_DONE);
        }
        // The inclusive loop uses a wider counter so a maximal persisted
        // height cannot wrap to zero and repeatedly replay the source.
        for (uint64_t cursor = static_cast<uint32_t>(start_height); cursor <= tip_height; ++cursor) {
            const auto height = static_cast<uint32_t>(cursor);
            std::optional<Block> legacy_body;
            if (!prepared) {
                const auto hash = chain_db->getBlockHashByHeight(height);
                if (!hash.ok()) throw std::runtime_error("Block rescan source height unavailable");
                const auto source = dinero::storage::ReadArchivalBlock(*chain_db, block_storage, *hash);
                if (!source.ok()) throw std::runtime_error("Block rescan source body unavailable");
                legacy_body = *source;
            }
            const auto& block = prepared ? prepared->blocks_[height] : *legacy_body;
            for (size_t tx_index = 0; tx_index < block.vtx.size(); ++tx_index) {
                const auto& tx = block.vtx[tx_index];
                const std::string txid = tx.GetTxid().AsUint256().GetHex();
                const bool coinbase = tx_index == 0;
                for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
                    const auto& output = tx.vout[vout];
                    if (!scripts.count(output.scriptPubKey)) continue;
                    if (output.value.GetUna() > uint64_t(INT64_MAX) || vout > UINT32_MAX)
                        throw std::runtime_error("Block rescan owned output out of range");
                    std::string script;
                    static constexpr char hex[] = "0123456789abcdef";
                    script.reserve(output.scriptPubKey.size() * 2);
                    for (const auto byte : output.scriptPubKey) {
                        script.push_back(hex[byte >> 4]); script.push_back(hex[byte & 15]);
                    }
                    std::string address = extractAddressFromScript(output.scriptPubKey);
                    if (address.empty()) address = "script:" + script.substr(0, 16);
                    auto row = prepare(R"(
                        INSERT OR IGNORE INTO utxos
                        (wallet_id,txid,vout,address,amount,script_pubkey,height,is_coinbase,is_spent,created_at)
                        VALUES(?,?,?,?,?,?,?,?,0,?)
                    )");
                    checked(sqlite3_bind_int(row.get(), 1, current_wallet_id_), SQLITE_OK);
                    checked(sqlite3_bind_text(row.get(), 2, txid.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
                    checked(sqlite3_bind_int64(row.get(), 3, vout), SQLITE_OK);
                    checked(sqlite3_bind_text(row.get(), 4, address.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
                    checked(sqlite3_bind_int64(row.get(), 5, output.value.GetUna()), SQLITE_OK);
                    checked(sqlite3_bind_text(row.get(), 6, script.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
                    checked(sqlite3_bind_int64(row.get(), 7, height), SQLITE_OK);
                    checked(sqlite3_bind_int(row.get(), 8, coinbase ? 1 : 0), SQLITE_OK);
                    checked(sqlite3_bind_int64(row.get(), 9, std::time(nullptr)), SQLITE_OK);
                    checked(sqlite3_step(row.get()), SQLITE_DONE);
                }
                if (!coinbase) {
                    for (const auto& input : tx.vin) {
                        const auto previous = input.prevout.txid.AsUint256().GetHex();
                        auto row = prepare(R"(
                            UPDATE utxos SET is_spent=1,spent_txid=?,spent_height=?
                            WHERE wallet_id=? AND txid=? AND vout=? AND is_spent=0
                        )");
                        checked(sqlite3_bind_text(row.get(), 1, txid.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
                        checked(sqlite3_bind_int64(row.get(), 2, height), SQLITE_OK);
                        checked(sqlite3_bind_int(row.get(), 3, current_wallet_id_), SQLITE_OK);
                        checked(sqlite3_bind_text(row.get(), 4, previous.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
                        checked(sqlite3_bind_int64(row.get(), 5, input.prevout.vout), SQLITE_OK);
                        checked(sqlite3_step(row.get()), SQLITE_DONE);
                    }
                }
            }
            std::string error;
            if (!wallet::shielded_ops::RescanConfirmedBlock(*this, height, block.vtx, &error))
                throw std::runtime_error("Block rescan shielded consumer failed: " + error);
            if ((cursor - start_height + 1) % 100 == 0 || height == tip_height) {
                auto progress = prepare(R"(
                    INSERT INTO sync_meta(id,last_scanned_height,birth_height,scan_complete) VALUES(1,?,?,?)
                    ON CONFLICT(id) DO UPDATE SET last_scanned_height=excluded.last_scanned_height,
                        birth_height=excluded.birth_height,scan_complete=excluded.scan_complete
                )");
                checked(sqlite3_bind_int64(progress.get(), 1, height), SQLITE_OK);
                checked(sqlite3_bind_int(progress.get(), 2, start_height), SQLITE_OK);
                checked(sqlite3_bind_int(progress.get(), 3, height == tip_height ? 1 : 0), SQLITE_OK);
                checked(sqlite3_step(progress.get()), SQLITE_DONE);
                if (sqlite3_changes(db_) != 1) throw std::runtime_error("Block rescan progress not stored");
            }
        }
        // Reserve memory publication ownership before committing the durable
        // tip. Nothing fallible remains between COMMIT and height publication.
        height_lock.lock();
        auto persisted_tip = prepare("INSERT INTO tip(rowid,height) VALUES(1,?) ON CONFLICT(rowid) DO UPDATE SET height=excluded.height");
        checked(sqlite3_bind_int64(persisted_tip.get(), 1, tip_height), SQLITE_OK);
        checked(sqlite3_step(persisted_tip.get()), SQLITE_DONE);
        if (sqlite3_changes(db_) != 1) throw std::runtime_error("Block rescan tip not stored");
        exec(db_, "COMMIT");
        owned_transaction = false;
    } catch (...) {
        if (owned_transaction && !sqlite3_get_autocommit(db_) &&
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_)) std::terminate();
        return false;
    }
    current_blockchain_height_ = tip_height;
    height_lock.unlock();
    height_cv_.notify_all();
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
    // Pin the selected database for the complete producer/consumer transaction.
    // The producer must already own immutable source data and must not acquire
    // chain locks or wait for another thread that needs this wallet lease.
    std::unique_ptr<DatabaseLease> database_lease;
    try { database_lease = AcquireDatabaseLease(); }
    catch (...) { return -1; }
    if (!db_ || current_wallet_id_ == -1) return -1;
    if (!sqlite3_get_autocommit(db_)) return -1; // Never adopt a caller transaction.

    auto checked = [&](int result, int expected) {
        if (result != expected)
            throw std::runtime_error(std::string("Snapshot wallet SQL failed: ") + sqlite3_errmsg(db_));
    };
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    auto prepare = [&](const char* sql) {
        sqlite3_stmt* raw = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
        Statement stmt(raw, sqlite3_finalize);
        checked(rc, SQLITE_OK);
        return stmt;
    };
    auto has_column = [&](const char* table, const char* column) {
        const std::string query = std::string("PRAGMA table_info(") + table + ")";
        auto stmt = prepare(query.c_str());
        bool found = false;
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            if (name && std::string_view(name) == column) found = true;
        }
        checked(rc, SQLITE_DONE);
        return found;
    };
    int recorded = 0;
    bool owned_transaction = false;
    // Reserve publication ownership before COMMIT; no fallible SQL or lock
    // acquisition may turn a committed import into an apparent failed import.
    std::unique_lock<std::mutex> height_lock(height_mu_, std::defer_lock);
    uint32_t published_height = 0;
    try {
        exec(db_, "PRAGMA synchronous=FULL");
        {
            auto policy = prepare("PRAGMA synchronous");
            checked(sqlite3_step(policy.get()), SQLITE_ROW);
            if (sqlite3_column_int(policy.get(), 0) != 2)
                throw std::runtime_error("Snapshot wallet durability unavailable");
            checked(sqlite3_step(policy.get()), SQLITE_DONE);
        }
        exec(db_, "BEGIN IMMEDIATE");
        owned_transaction = true;
        std::set<std::vector<uint8_t>> watch_scripts;
        {
            auto stmt = prepare("SELECT script_pubkey FROM watch_scripts");
            int rc;
            while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
                const auto* bytes = static_cast<const uint8_t*>(sqlite3_column_blob(stmt.get(), 0));
                const int size = sqlite3_column_bytes(stmt.get(), 0);
                if (sqlite3_column_type(stmt.get(), 0) != SQLITE_BLOB || !bytes || size <= 0)
                    throw std::runtime_error("Snapshot wallet watch script unavailable");
                watch_scripts.emplace(bytes, bytes + size);
            }
            checked(rc, SQLITE_DONE);
        }
        if (!has_column("utxos", "snapshot_anchored"))
            exec(db_, "ALTER TABLE utxos ADD COLUMN snapshot_anchored INTEGER NOT NULL DEFAULT 0");
        // Per-coin sink: identical insert path to rescanBlockchain (hex-encode the
        // scriptPubKey, extractAddressFromScript, upsert INTO utxos).
        auto sink = [&](const UtxoSetEntry& e) {
            if (watch_scripts.find(e.script_pubkey) == watch_scripts.end()) {
                return;  // not ours
            }

            if (e.height > snapshot_height || e.amount_una > uint64_t(INT64_MAX) ||
                e.txid_hex.size() != 64 || e.txid_hex.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("Snapshot owned coin fields are out of range");
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
            if (e.is_coinbase && uint64_t(snapshot_height) < uint64_t(e.height) + 100) {
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

            auto statement = prepare(insert_sql);
            auto* stmt = statement.get();

            int bind_index = 1;
            checked(sqlite3_bind_int(stmt, bind_index++, current_wallet_id_), SQLITE_OK);
            checked(sqlite3_bind_text(stmt, bind_index++, e.txid_hex.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
            checked(sqlite3_bind_int64(stmt, bind_index++, e.vout), SQLITE_OK);
            checked(sqlite3_bind_text(stmt, bind_index++, address.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
            checked(sqlite3_bind_int64(stmt, bind_index++, static_cast<int64_t>(e.amount_una)), SQLITE_OK);
            checked(sqlite3_bind_text(stmt, bind_index++, script_pubkey_hex.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
            checked(sqlite3_bind_int64(stmt, bind_index++, e.height), SQLITE_OK);
            checked(sqlite3_bind_int(stmt, bind_index++, e.is_coinbase ? 1 : 0), SQLITE_OK);
            checked(sqlite3_bind_int(stmt, bind_index++, is_mature), SQLITE_OK);
            checked(sqlite3_bind_int64(stmt, bind_index++, std::time(nullptr)), SQLITE_OK);

            checked(sqlite3_step(stmt), SQLITE_DONE);
            if (sqlite3_changes(db_) > 0) {
                if (recorded == INT_MAX) throw std::runtime_error("Snapshot owned coin count exceeded");
                ++recorded;
            }
        };
        produce(sink);
        height_lock.lock();
        published_height = std::max(current_blockchain_height_, snapshot_height);
        {
            auto progress = prepare("UPDATE sync_meta SET last_scanned_height=? WHERE id=1");
            checked(sqlite3_bind_int64(progress.get(), 1, snapshot_height), SQLITE_OK);
            checked(sqlite3_step(progress.get()), SQLITE_DONE);
            if (sqlite3_changes(db_) != 1)
                throw std::runtime_error("Snapshot wallet progress row unavailable");
        }
        if (snapshot_height > current_blockchain_height_) {
            auto tip = prepare("INSERT INTO tip(rowid,height) VALUES(1,?) ON CONFLICT(rowid) DO UPDATE SET height=excluded.height");
            checked(sqlite3_bind_int64(tip.get(), 1, published_height), SQLITE_OK);
            checked(sqlite3_step(tip.get()), SQLITE_DONE);
            // Keep the existing ordinary-wallet display maturity/confirmation
            // rules, but stage them with the imported coins and progress.
            const bool scoped = has_column("transactions", "wallet_id");
            if (has_column("transactions", "height") && has_column("transactions", "confirmations")) {
                auto tx = prepare(scoped
                    ? "UPDATE transactions SET confirmations=CASE WHEN ? >= height THEN ?-height+1 ELSE confirmations END WHERE height>0 AND wallet_id=?"
                    : "UPDATE transactions SET confirmations=CASE WHEN ? >= height THEN ?-height+1 ELSE confirmations END WHERE height>0");
                checked(sqlite3_bind_int64(tx.get(), 1, published_height), SQLITE_OK);
                checked(sqlite3_bind_int64(tx.get(), 2, published_height), SQLITE_OK);
                if (scoped) checked(sqlite3_bind_int(tx.get(), 3, current_wallet_id_), SQLITE_OK);
                checked(sqlite3_step(tx.get()), SQLITE_DONE);
            }
            auto maturity = prepare("UPDATE utxos SET is_mature=CASE WHEN is_coinbase=0 OR ?-height+1>=100 THEN 1 ELSE 0 END WHERE wallet_id=? AND is_spent=0");
            checked(sqlite3_bind_int64(maturity.get(), 1, published_height), SQLITE_OK);
            checked(sqlite3_bind_int(maturity.get(), 2, current_wallet_id_), SQLITE_OK);
            checked(sqlite3_step(maturity.get()), SQLITE_DONE);
        }
        exec(db_, "COMMIT");
        owned_transaction = false;
    } catch (...) {
        if (owned_transaction && !sqlite3_get_autocommit(db_) &&
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_)) std::terminate();
        return -1;
    }
    current_blockchain_height_ = published_height;
    height_lock.unlock();
    height_cv_.notify_all();
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
        INSERT INTO utxos
        (wallet_id, txid, vout, address, amount, script_pubkey, height, is_coinbase, is_spent, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?)
        ON CONFLICT DO UPDATE SET
            address=excluded.address, amount=excluded.amount,
            script_pubkey=excluded.script_pubkey, height=excluded.height,
            is_coinbase=excluded.is_coinbase
        WHERE utxos.wallet_id=excluded.wallet_id
          AND utxos.txid=excluded.txid AND utxos.vout=excluded.vout
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
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE && changed == 1) {
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
    return deriveKeyForScriptPubKeyOwned(script_pubkey,nullptr);
}

std::optional<SigningKey> WalletManager::resolveSigningKeyForScriptPubKey(const std::string& script_pubkey) {
    std::vector<uint8_t> script;
    if(!util::unhex(script_pubkey,script) || script.empty())return std::nullopt;
    auto policy=script.size()==34 && script[0]==0x51 && script[1]==0x20
        ?SigningKeyPolicy::TaprootCanonical:SigningKeyPolicy::Untweaked;
    auto key=deriveKeyForScriptPubKeyOwned(script_pubkey,&policy);
    if(!key)return std::nullopt;
    return SigningKey(std::move(*key),std::move(script),policy);
}

std::optional<std::vector<uint8_t>> WalletManager::deriveKeyForScriptPubKeyOwned(
    const std::string& script_pubkey,SigningKeyPolicy* policy,bool pinned_signing) {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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

    // Descriptor imports carry an internal key, not a BIP32 path. Resolve
    // their durable tuple before the HD cache/fallback can invent authority.
    std::vector<uint8_t> imported_script;
    if (util::unhex(script_pubkey, imported_script) && imported_script.size()==34 &&
        imported_script[0]==0x51 && imported_script[1]==0x20) {
        try {
            auto lease=AcquireDatabaseLease();
            if (!db_ || !sqlite3_get_autocommit(db_) || (recovery_seeds_ && !pinned_signing)) return std::nullopt;
            if(!pinned_signing)checkUnlockTimeout();
            if (wallet_locked_) return std::nullopt;
            // The existing transaction owner also gives all tuple/policy reads
            // one SQLite snapshot. It changes no wallet rows or receipts here.
            IssuedAddressTransaction read(db_);
            const auto text_matches=[](sqlite3_stmt* q,int col,const std::string& expected) {
                if(sqlite3_column_type(q,col)!=SQLITE_TEXT)return false;
                const auto* value=sqlite3_column_text(q,col);
                return value &&
                    sqlite3_column_bytes(q,col)==int(expected.size()) &&
                    std::memcmp(value,expected.data(),expected.size())==0;
            };
            const auto int_matches=[](sqlite3_stmt* q,int col,int expected) {
                return sqlite3_column_type(q,col)==SQLITE_INTEGER && sqlite3_column_int64(q,col)==expected;
            };
            std::array<uint8_t,32> output{};
            std::copy(imported_script.begin()+2,imported_script.end(),output.begin());
            const auto& network=Params().name;
            const auto address=TaprootKeys::CreateTaprootAddress(output,network=="regtest"?"rdin":network=="testnet"?"tdin":"din");
            const auto canonical_script=util::hex(imported_script);
            bool imported=false,has_keys=false;
            { IssuedStatement candidate(db_,"SELECT 1 FROM addresses WHERE (address=? OR script_pubkey=?) AND account=-1 UNION ALL SELECT 1 FROM watch_scripts WHERE script_pubkey=? AND path LIKE 'tr(%'");
              candidate.Text(1,address);candidate.Text(2,canonical_script);candidate.Blob(3,imported_script.data(),34);
              int rc;while((rc=sqlite3_step(candidate.value.get()))==SQLITE_ROW)imported=true;
              IssuanceCheck(db_,rc,SQLITE_DONE); }
            { IssuedStatement tables(db_,"SELECT 1 FROM sqlite_schema WHERE type='table' AND name='taproot_keys'");
              const int rc=sqlite3_step(tables.value.get());
              if(rc==SQLITE_ROW){has_keys=true;tables.Done();}else IssuanceCheck(db_,rc,SQLITE_DONE); }
            if(has_keys) {
                IssuedStatement candidate(db_,"SELECT 1 FROM taproot_keys WHERE address=? OR output_pubkey=?");
                candidate.Text(1,address);candidate.Blob(2,output.data(),32);
                int rc;while((rc=sqlite3_step(candidate.value.get()))==SQLITE_ROW)imported=true;
                IssuanceCheck(db_,rc,SQLITE_DONE);
            }
            // The predecessor import format is an internal scalar under a
            // different public tweak. Resolve it in this snapshot, before any
            // cached scalar or HD label can bypass its durable owner.
            const auto historical_address=AddressCodec::encodeP2TR(
                Network::MAIN,std::vector<uint8_t>(output.begin(),output.end()));
            IssuedStatement legacy(db_,"SELECT private_key_enc FROM imported_keys WHERE address=?");
            legacy.Text(1,historical_address);
            const int legacy_rc=sqlite3_step(legacy.value.get());
            if(legacy_rc==SQLITE_ROW) {
                if(imported) return std::nullopt; // conflicting modern owner
                struct Secret {std::string value;~Secret(){secureClearString(value);}} stored,plain;
                if(sqlite3_column_type(legacy.value.get(),0)!=SQLITE_TEXT)return std::nullopt;
                const auto* bytes=static_cast<const char*>(sqlite3_column_blob(legacy.value.get(),0));
                const int size=sqlite3_column_bytes(legacy.value.get(),0);
                if(!bytes || (wallet_encrypted_?(size!=60 && size!=92):size!=64))return std::nullopt;
                stored.value.assign(bytes,size);
                legacy.Done();
                { IssuedStatement policy(db_,"SELECT value FROM settings WHERE key='wallet_encrypted'");
                  const int rc=sqlite3_step(policy.value.get());
                  if(rc==SQLITE_ROW){if(!text_matches(policy.value.get(),0,wallet_encrypted_?"1":"0"))return std::nullopt;policy.Done();}
                  else {IssuanceCheck(db_,rc,SQLITE_DONE);if(wallet_encrypted_)return std::nullopt;} }
                { IssuedStatement metadata(db_,"SELECT encrypted FROM encryption_metadata WHERE id=1");
                  const int rc=sqlite3_step(metadata.value.get());
                  if(rc==SQLITE_ROW){if(!int_matches(metadata.value.get(),0,wallet_encrypted_?1:0))return std::nullopt;metadata.Done();}
                  else {IssuanceCheck(db_,rc,SQLITE_DONE);if(wallet_encrypted_)return std::nullopt;} }
                if(wallet_encrypted_ && encryption_key_.size()!=32)return std::nullopt;
                plain.value=wallet_encrypted_?decryptData(stored.value,encryption_key_):stored.value;
                struct Bytes {std::vector<uint8_t> value;~Bytes(){secureClearBytes(value);}} decoded;
                if(wallet_encrypted_ && plain.value.size()==32)
                    decoded.value.assign(plain.value.begin(),plain.value.end());
                else if(plain.value.size()!=64 || !util::unhex(plain.value,decoded.value) || decoded.value.size()!=32)
                    return std::nullopt;
                struct Scalar {std::array<uint8_t,32> value{};~Scalar(){OPENSSL_cleanse(value.data(),value.size());}} scalar;
                std::copy(decoded.value.begin(),decoded.value.end(),scalar.value.begin());
                std::array<uint8_t,32> internal{},derived_output{},tweak{};int parity=0;
                if(!TaprootKeys::DeriveXOnlyPubkey(scalar.value,internal,parity))return std::nullopt;
                std::array<uint8_t,33> tweak_input{};
                std::copy(internal.begin(),internal.end(),tweak_input.begin());
                ::SHA256(tweak_input.data(),tweak_input.size(),tweak.data());
                std::unique_ptr<secp256k1_context,decltype(&secp256k1_context_destroy)> context(
                    secp256k1_context_create(SECP256K1_CONTEXT_VERIFY),secp256k1_context_destroy);
                secp256k1_xonly_pubkey point{},result{};secp256k1_pubkey tweaked{};
                if(!context || !secp256k1_xonly_pubkey_parse(context.get(),&point,internal.data()) ||
                   !secp256k1_xonly_pubkey_tweak_add(context.get(),&tweaked,&point,tweak.data()) ||
                   !secp256k1_xonly_pubkey_from_pubkey(context.get(),&result,nullptr,&tweaked) ||
                   !secp256k1_xonly_pubkey_serialize(context.get(),derived_output.data(),&result) ||
                   derived_output!=output)return std::nullopt;
                read.Commit();
                if(policy)*policy=SigningKeyPolicy::TaprootHistoricalImport;
                // The compatibility API still returns the INTERNAL scalar.
                // Typed callers receive the separately carried tweak policy.
                return std::vector<uint8_t>(scalar.value.begin(),scalar.value.end());
            }
            IssuanceCheck(db_,legacy_rc,SQLITE_DONE);
            if(imported) {
                // Missing, old incomplete, or conflicting tuples refuse. Lookup
                // never backfills a mapping or treats an import as HD account0.
                if(!has_keys)return std::nullopt;
                const bool wallet_column=IssuanceWalletColumn(db_,"addresses");
                const std::string sql=R"(SELECT k.internal_privkey,k.internal_pubkey,k.output_pubkey,k.is_privkey_encrypted,
                    m.internal_pubkey,m.derivation_path,w.path,w.is_change,a.account,a.change,a.type,a.script_pubkey,)"+
                    std::string(wallet_column?"a.wallet_id":"1")+R"( FROM taproot_keys k
                    JOIN taproot_key_mapping m ON m.output_pubkey=k.output_pubkey
                    JOIN watch_scripts w ON w.script_pubkey=? JOIN addresses a ON a.address=k.address
                    WHERE k.address=?)";
                IssuedStatement tuple(db_,sql.c_str());tuple.Blob(1,imported_script.data(),34);tuple.Text(2,address);
                IssuanceCheck(db_,sqlite3_step(tuple.value.get()),SQLITE_ROW);auto* q=tuple.value.get();
                const auto key_blob=[&](int col,std::array<uint8_t,32>& dest) {
                    if(sqlite3_column_type(q,col)!=SQLITE_BLOB)throw std::runtime_error("Invalid imported public key type");
                    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q,col));
                    if(!bytes || sqlite3_column_bytes(q,col)!=32)
                        throw std::runtime_error("Invalid imported public key");
                    std::copy(bytes,bytes+32,dest.begin());
                };
                std::array<uint8_t,32> internal{},stored_output{},mapped{};
                key_blob(1,internal);key_blob(2,stored_output);key_blob(4,mapped);
                const auto path="tr("+util::hex(std::vector<uint8_t>(internal.begin(),internal.end())).substr(0,8)+"...)";
                if(stored_output!=output || mapped!=internal || !text_matches(q,5,path) || !text_matches(q,6,path) ||
                   !int_matches(q,7,0) || !int_matches(q,8,-1) || !int_matches(q,9,0) ||
                   !text_matches(q,10,"p2tr") || !text_matches(q,11,canonical_script) || !int_matches(q,12,1) ||
                   !int_matches(q,3,wallet_encrypted_?1:0))return std::nullopt;
                struct Secret {std::string value;~Secret(){secureClearString(value);}} stored,plain;
                if(sqlite3_column_type(q,0)!=SQLITE_BLOB)return std::nullopt;
                const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q,0));
                const int size=sqlite3_column_bytes(q,0);
                if(!bytes || size!=(wallet_encrypted_?60:32))return std::nullopt;
                stored.value.assign(bytes,size);tuple.Done();
                // Match the durable encryption policy, not merely the blob flag.
                { IssuedStatement policy(db_,"SELECT value FROM settings WHERE key='wallet_encrypted'");
                  const int rc=sqlite3_step(policy.value.get());
                  if(rc==SQLITE_ROW){if(!text_matches(policy.value.get(),0,wallet_encrypted_?"1":"0"))return std::nullopt;policy.Done();}
                  else {IssuanceCheck(db_,rc,SQLITE_DONE);if(wallet_encrypted_)return std::nullopt;} }
                { IssuedStatement metadata(db_,"SELECT encrypted FROM encryption_metadata WHERE id=1");
                  const int rc=sqlite3_step(metadata.value.get());
                  if(rc==SQLITE_ROW){if(!int_matches(metadata.value.get(),0,wallet_encrypted_?1:0))return std::nullopt;metadata.Done();}
                  else {IssuanceCheck(db_,rc,SQLITE_DONE);if(wallet_encrypted_)return std::nullopt;} }
                if(wallet_encrypted_ && encryption_key_.size()!=32)return std::nullopt;
                plain.value=wallet_encrypted_?decryptData(stored.value,encryption_key_):stored.value;
                if(plain.value.size()!=32)return std::nullopt;
                struct Key {std::array<uint8_t,32> value{};~Key(){OPENSSL_cleanse(value.data(),value.size());}} secret;
                std::copy(plain.value.begin(),plain.value.end(),secret.value.begin());
                std::array<uint8_t,32> derived{},tweaked{};int parity=0;
                if(!TaprootKeys::DeriveXOnlyPubkey(secret.value,derived,parity) || derived!=internal ||
                   !TaprootKeys::ComputeTweakedPubkey(derived,tweaked) || tweaked!=output)return std::nullopt;
                read.Commit();
                // Do not cache imported plaintext: every lookup must recheck the
                // persistent key, public bindings and current encryption owner.
                return std::vector<uint8_t>(secret.value.begin(),secret.value.end());
            }
            read.Commit();
        } catch(const std::exception&) {
            WLOG_ERR("Imported Taproot signing key lookup refused");
            return std::nullopt;
        }
    }

    // HD lookup authenticates the current durable tuple in one snapshot. A
    // plaintext cache, recognition label or inferred current-network path is
    // never a substitute for a recorded owner. No lookup repairs wallet rows.
    try {
        auto lease=AcquireDatabaseLease();
        if(!db_ || !sqlite3_get_autocommit(db_) || (recovery_seeds_ && !pinned_signing))return std::nullopt;
        if(!pinned_signing)checkUnlockTimeout();
        if(wallet_locked_ || master_seed_.size()!=64)return std::nullopt;
        std::vector<uint8_t> script;
        if(!util::unhex(script_pubkey,script) || script.empty())return std::nullopt;
        IssuedAddressTransaction read(db_);
        const auto path=AuthenticateHdInventory(db_,master_seed_,&script);
        if(!path)return std::nullopt;
        BIP32Deriver deriver(master_seed_.data(),master_seed_.size());
        for(size_t i=0;i<3;++i)deriver.deriveHardened((*path)[i]);
        deriver.deriveNormal((*path)[3]);deriver.deriveNormal((*path)[4]);
        struct Scalar {std::array<uint8_t,32> value;~Scalar(){OPENSSL_cleanse(value.data(),value.size());}} scalar{deriver.getPrivateKey()};
        struct Secret {std::vector<uint8_t> value;~Secret(){secureClearBytes(value);}} secret;
        secret.value.assign(scalar.value.begin(),scalar.value.end());
        read.Commit();
        return std::move(secret.value);
    } catch(const std::exception&) {
        WLOG_ERR("HD signing key lookup refused");return std::nullopt;
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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

std::string WalletManager::importPrivateKey(const std::vector<uint8_t>& privkey,
    const std::string& label, uint64_t expected_session, const std::string& expected_address) {
    std::lock_guard<std::recursive_mutex> owner(database_lifecycle_mutex_);
    if (privkey.size()!=32 || !hasActiveWallet()) return {};
    struct Secret {
        std::array<uint8_t,32> value{};
        ~Secret(){OPENSSL_cleanse(value.data(),value.size());}
    } key;
    std::copy(privkey.begin(),privkey.end(),key.value.begin());
    std::array<uint8_t,32> internal{},output{};int parity=0;
    if (!TaprootKeys::DeriveXOnlyPubkey(key.value,internal,parity) ||
        !TaprootKeys::ComputeTweakedPubkey(internal,output)) return {};
    const auto& network=Params().name;
    const auto address=TaprootKeys::CreateTaprootAddress(output,
        network=="regtest"?"rdin":network=="testnet"?"tdin":"din");
    if (address.empty() || (!expected_address.empty() && expected_address!=address)) return {};
    // The existing owner checks legacy inventory in the SAME transaction as
    // persistence. A backup's recorded address cannot silently be substituted.
    if (!storeTaprootKey(address,key.value,internal,output,label,expected_session,true)) return {};
    return address;
}

void WalletManager::cachePrivateKey(const std::string& address, const std::vector<uint8_t>& key) {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    // Store private key in cache (in memory only while wallet is unlocked)
    private_key_cache_[address] = key;
    WLOG_DEBUG("Cached private key for address: " + address);
}

void WalletManager::clearPrivateKeyCache() {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    return storeMasterSeedOwned(seed, passphrase, reset_address_state, nullptr);
}

bool WalletManager::storeMasterSeedOwned(const std::vector<uint8_t>& seed,
    const std::string& passphrase, bool reset_address_state, const std::string* initial_owner) {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (recovery_seeds_) throw std::logic_error("Wallet recovery key is pinned");
    if (!db_ || current_wallet_id_ < 0) {
        WLOG_ERR("No active wallet to store master seed");
        return false;
    }

    if (seed.size() != 64) {
        WLOG_ERR("Invalid seed size: " + std::to_string(seed.size()) + " (expected 64 bytes)");
        return false;
    }

    try {
        auto lease = AcquireDatabaseLease();
        IssuedAddressTransaction transaction(db_);
        if (initial_owner) {
            // Only creation supplies this sealed owner. Never replace an
            // established seed, even if its ciphertext cannot be read.
            IssuedStatement empty(db_, "SELECT 1 FROM hd_seeds");
            empty.Done();
            IssuedStatement owner(db_, "INSERT INTO settings(key,value,updated_at) VALUES(?,?,strftime('%s','now'))");
            owner.Text(1, kInitialOwnerSetting); owner.Text(2, *initial_owner); owner.Done(true);
        }
        struct SeedBuffer {
            std::vector<uint8_t> value;
            ~SeedBuffer() { secureClearBytes(value); }
        } previous{master_seed_}, staged{seed};

        // Re-importing the active recovery phrase is an idempotent wallet bind,
        // not a wallet replacement. In particular, embedded/mobile clients bind
        // on every process start and may explicitly skip address derivation after
        // the first successful bind. Clearing address state here would therefore
        // erase watch_scripts and make the otherwise valid retry unusable.
        if (previous.value.empty() && !wallet_locked_) {
            auto existing_seed = loadMasterSeed("");
            if (existing_seed.has_value()) {
                previous.value = std::move(existing_seed.value());
            }
        }
        const bool replaces_wallet_identity =
            reset_address_state && !ConstantTimeEqual(seed, previous.value);
        if(PaymentColumn(db_)) {
            if(!ConstantTimeEqual(seed,previous.value))
                throw std::runtime_error("Seed replacement would orphan retained payments");
            (void)ReadPendingPaymentsOwned(previous.value);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // Optional address-state reset
        // ═══════════════════════════════════════════════════════════════════════
        // Required when replacing the wallet seed (restore/import flows), but
        // is disabled for initial creation. This preserves the existing
        // replacement decision; it does not certify recovery completeness.
        // ═══════════════════════════════════════════════════════════════════════

        if (replaces_wallet_identity) {
            WLOG_INFO("Clearing address tables for new HD seed import...");

            // Preserve the existing replacement policy, but every required
            // deletion now belongs to the same transaction as the new seed.
            for (const char* table : {"addresses", "address_derivation_paths", "hd_address_book",
                                      "utxos", "watch_scripts", "taproot_key_mapping", "transactions"}) {
                const std::string name(table);
                if (name == "hd_address_book" || name == "taproot_key_mapping") {
                    IssuedStatement exists(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
                    exists.Text(1, name);
                    const int rc = sqlite3_step(exists.value.get());
                    if (rc == SQLITE_DONE) continue; // Optional in older schemas.
                    IssuanceCheck(db_, rc, SQLITE_ROW);
                    exists.Done();
                }
                const std::string sql = "DELETE FROM " + name;
                IssuanceCheck(db_, sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
            }

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
        uint8_t derived_key[64]{};  // PBKDF2-SHA512 gives 64 bytes, we use first 32 for AES-256
        uint8_t aes_key[32]{};
        struct KeyCleanup {
            uint8_t* derived;
            uint8_t* aes;
            ~KeyCleanup() { OPENSSL_cleanse(derived, 64); OPENSSL_cleanse(aes, 32); }
        } key_cleanup{derived_key, aes_key};
        dinero::crypto::PBKDF2_HMAC_SHA512(
            reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size(),
            salt.data(), salt.size(),
            PBKDF2_ITERATIONS,
            derived_key, sizeof(derived_key)
        );

        // Use first 32 bytes as AES-256 key
        std::memcpy(aes_key, derived_key, 32);

        // === STEP 3: Encrypt seed with AES-256-GCM ===
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            OPENSSL_cleanse(derived_key, sizeof(derived_key));
            OPENSSL_cleanse(aes_key, sizeof(aes_key));
            WLOG_ERR("Failed to create cipher context");
            return false;
        }

        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(ctx, EVP_CIPHER_CTX_free);
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
            IssuedStatement row(db_, R"(
                INSERT OR REPLACE INTO hd_seeds (id, encrypted_seed, salt, coin_type, encryption_version, created_at)
                VALUES (1, ?, ?, ?, 2, strftime('%s','now'))
            )");
            row.Blob(1, encrypted_blob.data(), static_cast<int>(encrypted_blob.size()));
            row.Blob(2, salt.data(), static_cast<int>(salt.size()));
            row.Int(3, static_cast<int>(dinero::consensus::DINERO_COIN_TYPE));
            row.Done(true);

            IssuedStatement metadata(db_, R"(
                INSERT OR REPLACE INTO encryption_metadata (
                    id, encrypted, kdf, kdf_iterations, cipher, salt, created_at, updated_at
                )
                VALUES (1, COALESCE((SELECT encrypted FROM encryption_metadata WHERE id = 1), 0),
                        'pbkdf2-hmac-sha512', 600000, 'AES-256-GCM', ?,
                        COALESCE((SELECT created_at FROM encryption_metadata WHERE id = 1), strftime('%s','now')),
                        strftime('%s','now'))
            )");
            metadata.Blob(1, salt.data(), static_cast<int>(salt.size()));
            metadata.Done(true);

            // Any operation that replaces the seed invalidates the old mnemonic
            // binding. Clear it after the seed write succeeds; encryption passes
            // reset_address_state=false because they preserve the same identity.
            if (replaces_wallet_identity) {
                const auto set = [&](const char* name, const char* value) {
                    IssuedStatement setting(db_, "INSERT OR REPLACE INTO settings(key,value,updated_at) VALUES(?,?,strftime('%s','now'))");
                    setting.Text(1, name); setting.Text(2, value); setting.Done(true);
                };
                set(kBip39RecoverySetting, "");
                set(kBip39BackupAcknowledgedSetting, "0");
            }

            // Allocate the live seed before writing and publish only after the
            // required rows and commit succeed. The old live seed is cleansed.
            transaction.Commit();
            master_seed_.swap(staged.value);

            success = true;

        } while (false);

        // Cleanup
        OPENSSL_cleanse(derived_key, sizeof(derived_key));
        OPENSSL_cleanse(aes_key, sizeof(aes_key));

        return success;
    } catch (const std::exception& e) {
        WLOG_ERR(std::string("Failed to persist HD wallet seed: ") + e.what());
        return false;
    }
}

bool WalletManager::storeAuthoritativeBip39Mnemonic(
    const std::string& mnemonic,
    const std::string& bip39_passphrase,
    std::string* error_out) {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (!db_ || current_wallet_id_ < 0) {
        WLOG_ERR("No active wallet to load master seed from");
        return std::nullopt;
    }

    // Capture the established envelope in one checked statement before deriving
    // any secret. This read may borrow the caller's transaction, never commit it.
    std::vector<uint8_t> encrypted_blob;
    int64_t encryption_version = 0;
    {
        sqlite3_stmt* raw = nullptr;
        const int prepared = sqlite3_prepare_v2(
            db_, "SELECT encrypted_seed, encryption_version FROM hd_seeds WHERE id = 1",
            -1, &raw, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (prepared != SQLITE_OK || sqlite3_step(stmt.get()) != SQLITE_ROW)
            return std::nullopt;
        const int version_type = sqlite3_column_type(stmt.get(), 1);
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_BLOB ||
            sqlite3_column_bytes(stmt.get(), 0) != 124 ||
            (version_type != SQLITE_INTEGER && version_type != SQLITE_NULL))
            return std::nullopt;
        encryption_version = version_type == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt.get(), 1);
        // Only the documented absent/zero legacy marker may probe both KDFs.
        if (encryption_version < 0 || encryption_version > 2)
            return std::nullopt;
        const auto* bytes = static_cast<const uint8_t*>(sqlite3_column_blob(stmt.get(), 0));
        if (!bytes) return std::nullopt;
        encrypted_blob.assign(bytes, bytes + 124);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE)
            return std::nullopt;
    }

    // === STEP 2: Extract components from encrypted blob ===
    // Format: salt(32) + nonce(12) + ciphertext(64) + tag(16) = 124 bytes
    constexpr size_t SALT_SIZE = 32;
    constexpr size_t NONCE_SIZE = 12;
    constexpr size_t TAG_SIZE = 16;

    // Version-dispatched iteration count:
    //   Version 2 (current) = 600,000 PBKDF2-HMAC-SHA512 iterations
    //   Version 1 (legacy)  = 100,000 PBKDF2-HMAC-SHA512 iterations
    //   Legacy unmarked envelope (0 or NULL): authenticate with both known KDFs
    std::vector<uint32_t> iteration_candidates;
    if (encryption_version == 2) {
        iteration_candidates = {600000};
    } else if (encryption_version == 1) {
        iteration_candidates = {100000};
    } else {
        // Explicit legacy-unmarked version only; unsupported versions refused above.
        iteration_candidates = {600000, 100000};
        WLOG_INFO("Legacy unmarked seed envelope: trying known iteration counts");
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
    const auto database_lease = AcquireDatabaseLease();
    if (!hasActiveWallet()) return;
    if (height == 0 || height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Wallet disconnect height is out of range");

    // Failed BEGIN must leave an existing caller transaction untouched. This
    // transaction covers this ordinary wallet only; the worker's index and
    // shielded store are separate and may already have committed rollback.
    exec(db_, "BEGIN IMMEDIATE");
    try {
        auto checked = [&](int result, int expected, const char* phase) {
            if (result != expected)
                throw std::runtime_error(std::string("Wallet disconnect ") + phase +
                                         " failed: " + sqlite3_errmsg(db_));
        };
        using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
        auto prepare = [&](const char* sql, const char* phase) {
            sqlite3_stmt* raw = nullptr;
            const int result = sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
            Statement stmt(raw, sqlite3_finalize);
            checked(result, SQLITE_OK, phase);
            return stmt;
        };
        {
            auto stmt = prepare("DELETE FROM utxos WHERE height = ?", "delete prepare");
            checked(sqlite3_bind_int(stmt.get(), 1, static_cast<int>(height)), SQLITE_OK, "delete bind");
            checked(sqlite3_step(stmt.get()), SQLITE_DONE, "delete");
        }
        {
            // Keep the historical per-wallet schema compatibility of the old
            // history-removal path, but check every statement and binding.
            const bool scoped = IssuanceWalletColumn(db_, "transactions");
            UnconfirmOutgoingHistoryAtHeight(db_,current_wallet_id_,height,scoped);
            auto stmt = prepare(scoped
                ? "DELETE FROM transactions WHERE wallet_id = ? AND height = ?"
                : "DELETE FROM transactions WHERE height = ?", "history prepare");
            int parameter = 1;
            if (scoped)
                checked(sqlite3_bind_int(stmt.get(), parameter++, current_wallet_id_), SQLITE_OK, "history bind");
            checked(sqlite3_bind_int(stmt.get(), parameter, static_cast<int>(height)), SQLITE_OK, "history bind");
            checked(sqlite3_step(stmt.get()), SQLITE_DONE, "history");
        }
        {
            auto stmt = prepare("UPDATE utxos SET is_spent = 0 WHERE txid = ? AND vout = ? AND is_spent = 1", "restore prepare");
            for (const auto& tx : block.vtx) {
                if (tx.IsCoinbase()) continue;
                for (const auto& input : tx.vin) {
                    const std::string prev_txid = input.prevout.txid.AsUint256().GetHex();
                    checked(sqlite3_reset(stmt.get()), SQLITE_OK, "restore reset");
                    checked(sqlite3_bind_text(stmt.get(), 1, prev_txid.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK, "restore bind");
                    checked(sqlite3_bind_int64(stmt.get(), 2, input.prevout.vout), SQLITE_OK, "restore bind");
                    checked(sqlite3_step(stmt.get()), SQLITE_DONE, "restore");
                }
            }
        }
        checked(sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK, "commit");
    } catch (...) {
        if (!sqlite3_get_autocommit(db_) &&
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK &&
            !sqlite3_get_autocommit(db_)) std::terminate();
        throw;
    }
    // This remains a later publication/metadata operation, not a durable source
    // acknowledgment. Never lower the visible height after a failed SQL group.
    setBlockchainHeight(height - 1);
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
    auto lease = AcquireDatabaseLease();
    if (!utxo_index_) {
        dinero::g_logger.warning("[WalletManager] UTXOIndex not set - cannot load addresses");
        return; // An absent optional index is not an acknowledgment of recovery.
    }
    if (!db_) throw std::runtime_error("Wallet script inventory database unavailable");
    IssuedAddressTransaction read(db_);
    const auto text = [](sqlite3_stmt* q, int col) {
        if (sqlite3_column_type(q,col)!=SQLITE_TEXT)
            throw std::runtime_error("Invalid wallet script inventory text");
        const auto* value=static_cast<const char*>(sqlite3_column_blob(q,col));
        const int size=sqlite3_column_bytes(q,col);
        if (!value || size<=0 || std::memchr(value,0,size))
            throw std::runtime_error("Empty or malformed wallet script inventory text");
        return std::string(value,size);
    };
    const auto integer = [](sqlite3_stmt* q,int col) {
        if (sqlite3_column_type(q,col)!=SQLITE_INTEGER)
            throw std::runtime_error("Invalid wallet script inventory integer");
        return sqlite3_column_int64(q,col);
    };
    const auto parsed_script = [&](sqlite3_stmt* q,int col) {
        std::vector<uint8_t> script;
        if (!util::unhex(text(q,col),script) || script.empty())
            throw std::runtime_error("Invalid wallet address script");
        return script;
    };
    std::map<std::vector<uint8_t>,std::pair<std::string,sqlite3_int64>> captured;
    const auto add = [&](const std::vector<uint8_t>& script,const std::string& path,sqlite3_int64 change) {
        if (change!=0 && change!=1) throw std::runtime_error("Invalid wallet script change flag");
        const auto [it,inserted]=captured.emplace(script,std::make_pair(path,change));
        if (!inserted && it->second!=std::make_pair(path,change))
            throw std::runtime_error("Conflicting persistent wallet script path");
    };
    {
        IssuedStatement q(db_,"SELECT script_pubkey,path,is_change FROM watch_scripts");
        int rc;
        while ((rc=sqlite3_step(q.value.get()))==SQLITE_ROW) {
            auto* row=q.value.get();
            if (sqlite3_column_type(row,0)!=SQLITE_BLOB || sqlite3_column_bytes(row,0)<=0 || !sqlite3_column_blob(row,0))
                throw std::runtime_error("Invalid watched script");
            const auto* raw=static_cast<const uint8_t*>(sqlite3_column_blob(row,0));
            const std::vector<uint8_t> script(raw,raw+sqlite3_column_bytes(row,0));
            add(script,text(row,1),integer(row,2));
        }
        IssuanceCheck(db_,rc,SQLITE_DONE);
    }
    // The same known-script domain used by ordinary delivery. Empty/missing
    // address scripts and orphan descriptors still need independent discovery.
    // An existing watch row never hides another address's explicit path record.
    const bool wallet_column=IssuanceWalletColumn(db_,"addresses");
    const std::string sql=R"(SELECT a.script_pubkey,a.account,a.change,a.idx,
        p.derivation_path,p.script_pubkey,p.account,p.change,p.address_index,)"+
        std::string(wallet_column?"a.wallet_id":"1")+R"( FROM addresses a
        LEFT JOIN address_derivation_paths p ON p.address=a.address
        WHERE a.script_pubkey IS NOT NULL AND a.script_pubkey<>'')";
    {
        IssuedStatement q(db_,sql.c_str());int rc;
        while ((rc=sqlite3_step(q.value.get()))==SQLITE_ROW) {
            auto* row=q.value.get();const auto script=parsed_script(row,0);
            const auto account=integer(row,1),change=integer(row,2),index=integer(row,3);
            if (integer(row,9)!=1 || account < -1 || index<0 || index>INT_MAX || (change!=0 && change!=1))
                throw std::runtime_error("Invalid wallet address ownership metadata");
            if (sqlite3_column_type(row,4)!=SQLITE_NULL) {
                const auto path=text(row,4);
                if (parsed_script(row,5)!=script || integer(row,6)!=account ||
                    integer(row,7)!=change || integer(row,8)!=index)
                    throw std::runtime_error("Conflicting wallet address path metadata");
                add(script,path,change);
            } else {
                const auto watched=captured.find(script);
                if (watched==captured.end() || watched->second.second!=change)
                    throw std::runtime_error("Wallet address has no consistent recorded script path");
            }
        }
        IssuanceCheck(db_,rc,SQLITE_DONE);
    }
    std::map<std::vector<uint8_t>,std::string> scripts;
    for (const auto& [script,record] : captured) scripts.emplace(script,record.first);
    read.Commit(); // No SQL backfill, invented HD path, or receipt mutation.
    // The wallet/session remains pinned. The index stages a copy under its own
    // script mutex and swaps only after every existing binding agrees.
    utxo_index_->MergeRegisteredAddresses(scripts);
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

bool WalletManager::storeTaprootKey(const std::string& address,
                                    const std::array<uint8_t, 32>& internal_privkey,
                                    const std::array<uint8_t, 32>& internal_pubkey,
                                    const std::array<uint8_t, 32>& output_pubkey,
                                    const std::string& label, uint64_t expected_session, bool require_empty_legacy_imports) {
    try {
        auto lease = AcquireDatabaseLease();
        if (!db_ || !sqlite3_get_autocommit(db_) || recovery_seeds_ ||
            (expected_session && lease->Session()!=expected_session)) return false;
        checkUnlockTimeout();
        IssuedAddressTransaction transaction(db_);
        if (require_empty_legacy_imports) {
            IssuedStatement legacy(db_, "SELECT 1 FROM imported_keys LIMIT 1");
            const int result=sqlite3_step(legacy.value.get());
            if (result==SQLITE_ROW) return false;
            IssuanceCheck(db_,result,SQLITE_DONE);
        }
        const auto text_matches=[](sqlite3_stmt* q,int column,std::string_view expected) {
            const auto* value=sqlite3_column_text(q,column);
            return sqlite3_column_type(q,column)==SQLITE_TEXT && value &&
                sqlite3_column_bytes(q,column)==int(expected.size()) &&
                std::memcmp(value,expected.data(),expected.size())==0;
        };
        // Read the durable policy while the real wallet session is pinned.
        IssuedStatement policy(db_, "SELECT value FROM settings WHERE key='wallet_encrypted'");
        int rc = sqlite3_step(policy.value.get());
        bool encrypted = false;
        const bool policy_present=rc==SQLITE_ROW;
        if (rc == SQLITE_ROW) {
            if (!text_matches(policy.value.get(),0,"0") && !text_matches(policy.value.get(),0,"1")) return false;
            encrypted = text_matches(policy.value.get(),0,"1");
            policy.Done();
        } else IssuanceCheck(db_,rc,SQLITE_DONE);
        { IssuedStatement metadata(db_,"SELECT encrypted FROM encryption_metadata WHERE id=1");
          rc=sqlite3_step(metadata.value.get());
          if(rc==SQLITE_ROW) {
              const int flag=sqlite3_column_int(metadata.value.get(),0);
              if(sqlite3_column_type(metadata.value.get(),0)!=SQLITE_INTEGER || (flag!=0 && flag!=1) ||
                 (policy_present && encrypted!=(flag==1)))return false;
              encrypted=flag==1;metadata.Done();
          } else IssuanceCheck(db_,rc,SQLITE_DONE); }
        if (encrypted != wallet_encrypted_ || (encrypted && (wallet_locked_ || encryption_key_.size()!=32))) return false;

        std::array<uint8_t,32> derived_internal{}, derived_output{}; int parity=0;
        if (!TaprootKeys::DeriveXOnlyPubkey(internal_privkey,derived_internal,parity) ||
            derived_internal!=internal_pubkey ||
            !TaprootKeys::ComputeTweakedPubkey(internal_pubkey,derived_output) || derived_output!=output_pubkey) return false;
        const auto& network=Params().name;
        const std::string hrp=network=="regtest"?"rdin":network=="testnet"?"tdin":"din";
        if (TaprootKeys::CreateTaprootAddress(output_pubkey,hrp)!=address) return false;
        std::vector<uint8_t> script{0x51,0x20};script.insert(script.end(),output_pubkey.begin(),output_pubkey.end());
        const std::string path="tr("+util::hex(std::vector<uint8_t>(internal_pubkey.begin(),internal_pubkey.end())).substr(0,8)+"...)";
        const std::string script_hex=util::hex(script);
        const auto now=std::time(nullptr);
        exec(db_, R"(CREATE TABLE IF NOT EXISTS taproot_keys (
            id INTEGER PRIMARY KEY AUTOINCREMENT, address TEXT UNIQUE NOT NULL,
            internal_privkey BLOB NOT NULL, internal_pubkey BLOB NOT NULL, output_pubkey BLOB NOT NULL,
            label TEXT, created_at INTEGER NOT NULL, is_privkey_encrypted INTEGER NOT NULL DEFAULT 0))");
        bool has_encryption_flag=false;
        { IssuedStatement columns(db_,"PRAGMA table_info(taproot_keys)");
          while ((rc=sqlite3_step(columns.value.get()))==SQLITE_ROW) {
              const auto* name=sqlite3_column_text(columns.value.get(),1);
              if(!name) throw std::runtime_error("Imported key schema unavailable");
              has_encryption_flag |= std::string_view(reinterpret_cast<const char*>(name))=="is_privkey_encrypted";
          }
          IssuanceCheck(db_,rc,SQLITE_DONE); }
        if(!has_encryption_flag)exec(db_,"ALTER TABLE taproot_keys ADD COLUMN is_privkey_encrypted INTEGER NOT NULL DEFAULT 0");
        exec(db_, R"(CREATE TABLE IF NOT EXISTS taproot_key_mapping (
            output_pubkey BLOB PRIMARY KEY, internal_pubkey BLOB NOT NULL,
            derivation_path TEXT, created_at INTEGER NOT NULL))");
        { IssuedStatement watch(db_,"INSERT OR IGNORE INTO watch_scripts(script_pubkey,path,is_change,last_seen_height,created_at) VALUES(?,?,0,0,?)");
          watch.Blob(1,script.data(),int(script.size()));watch.Text(2,path);watch.Int(3,now);watch.Done();
          IssuedStatement verify(db_,"SELECT path,is_change FROM watch_scripts WHERE script_pubkey=?");
          verify.Blob(1,script.data(),int(script.size()));IssuanceCheck(db_,sqlite3_step(verify.value.get()),SQLITE_ROW);
          if(!text_matches(verify.value.get(),0,path) || sqlite3_column_type(verify.value.get(),1)!=SQLITE_INTEGER || sqlite3_column_int(verify.value.get(),1)!=0)
              throw std::runtime_error("Imported key watch path conflict");
          verify.Done(); }
        { IssuedStatement mapping(db_,"INSERT OR IGNORE INTO taproot_key_mapping(output_pubkey,internal_pubkey,derivation_path,created_at) VALUES(?,?,?,?)");
          mapping.Blob(1,output_pubkey.data(),32);mapping.Blob(2,internal_pubkey.data(),32);mapping.Text(3,path);mapping.Int(4,now);mapping.Done();
          IssuedStatement verify(db_,"SELECT internal_pubkey,derivation_path FROM taproot_key_mapping WHERE output_pubkey=?");
          verify.Blob(1,output_pubkey.data(),32);IssuanceCheck(db_,sqlite3_step(verify.value.get()),SQLITE_ROW);
          const auto* key=static_cast<const uint8_t*>(sqlite3_column_blob(verify.value.get(),0));
          if(!key || sqlite3_column_type(verify.value.get(),0)!=SQLITE_BLOB || sqlite3_column_bytes(verify.value.get(),0)!=32 ||
              !std::equal(internal_pubkey.begin(),internal_pubkey.end(),key) || !text_matches(verify.value.get(),1,path))throw std::runtime_error("Imported key mapping conflict");
          verify.Done(); }
        // Cleansed on every exit, including encryption and SQLite failures.
        struct Secret { std::string value; ~Secret(){secureClearString(value);} } raw, stored;
        raw.value.assign(internal_privkey.begin(),internal_privkey.end());
        stored.value=encrypted?encryptData(raw.value,encryption_key_):raw.value;
        { IssuedStatement key(db_, R"(INSERT INTO taproot_keys(address,internal_privkey,internal_pubkey,output_pubkey,label,created_at,is_privkey_encrypted)
            VALUES(?,?,?,?,?,?,?) ON CONFLICT(address) DO UPDATE SET internal_privkey=excluded.internal_privkey,
            label=excluded.label,is_privkey_encrypted=excluded.is_privkey_encrypted
            WHERE internal_pubkey=excluded.internal_pubkey AND output_pubkey=excluded.output_pubkey)");
          key.Text(1,address);key.Blob(2,stored.value.data(),int(stored.value.size()));key.Blob(3,internal_pubkey.data(),32);
          key.Blob(4,output_pubkey.data(),32);key.Text(5,label);key.Int(6,now);key.Int(7,encrypted?1:0);key.Done(true); }
        bool exists=false;const bool wallet_column=IssuanceWalletColumn(db_,"addresses");
        { IssuedStatement existing(db_,wallet_column?
              "SELECT account,type,script_pubkey,wallet_id FROM addresses WHERE address=?":
              "SELECT account,type,script_pubkey,1 FROM addresses WHERE address=?");
          existing.Text(1,address);rc=sqlite3_step(existing.value.get());
          if(rc==SQLITE_ROW) {
              if(sqlite3_column_type(existing.value.get(),0)!=SQLITE_INTEGER || sqlite3_column_int(existing.value.get(),0)!=-1 ||
                 sqlite3_column_type(existing.value.get(),3)!=SQLITE_INTEGER || sqlite3_column_int(existing.value.get(),3)!=1 ||
                 (!text_matches(existing.value.get(),1,"p2tr") && !text_matches(existing.value.get(),1,"taproot_imported")) ||
                 (sqlite3_column_type(existing.value.get(),2)!=SQLITE_NULL && !text_matches(existing.value.get(),2,"") &&
                  !text_matches(existing.value.get(),2,script_hex)))throw std::runtime_error("Imported address ownership conflict");
              exists=true;existing.Done();
          } else IssuanceCheck(db_,rc,SQLITE_DONE); }
        if(!exists) {
            const int next=getNextAddressIndex(-1,0);
            IssuedStatement row(db_,wallet_column?
                "INSERT INTO addresses(wallet_id,account,change,idx,address,type,script_pubkey,label,created_at) VALUES(1,-1,0,?,?,'p2tr',?,?,?)":
                "INSERT INTO addresses(account,change,idx,address,type,script_pubkey,label,created_at) VALUES(-1,0,?,?,'p2tr',?,?,?)");
            row.Int(1,next);row.Text(2,address);row.Text(3,script_hex);row.Text(4,label);row.Int(5,now);row.Done(true);
        } else {
            IssuedStatement row(db_,"UPDATE addresses SET type='p2tr',script_pubkey=?,label=?,is_system_label=0 WHERE address=?");
            row.Text(1,script_hex);row.Text(2,label);row.Text(3,address);row.Done(true);
        }
        transaction.Commit();
        if(utxo_index_)utxo_index_->RegisterAddress(script,path);
        return true;
    } catch(const std::exception& error) {
        WLOG_ERR("[storeTaprootKey] Import refused: "+std::string(error.what()));
        return false;
    }
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
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
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    return !master_seed_.empty();
}

std::optional<std::vector<uint8_t>> WalletManager::GetMasterSeed() const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);
    if (master_seed_.empty()) {
        return std::nullopt;
    }
    return master_seed_;
}

std::optional<std::vector<uint8_t>> WalletManager::DerivePrivateKey(
    const wallet::KeyOriginInfo& origin) const {
    std::lock_guard<std::recursive_mutex> key_ownership(database_lifecycle_mutex_);

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
