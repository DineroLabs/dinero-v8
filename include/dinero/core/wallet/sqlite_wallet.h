#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include "compat/jsoncpp_compat.h"

namespace Dinero {
namespace Wallet {

// RAII Transaction helper for automatic rollback
class SQLiteTransaction {
public:
    explicit SQLiteTransaction(sqlite3* db);
    ~SQLiteTransaction();
    
    bool commit();
    void rollback();
    
    // Non-copyable
    SQLiteTransaction(const SQLiteTransaction&) = delete;
    SQLiteTransaction& operator=(const SQLiteTransaction&) = delete;
    
private:
    sqlite3* db;
    bool committed;
    bool rolled_back;
};

// RAII Statement helper for automatic finalization
class SQLiteStatement {
public:
    SQLiteStatement(sqlite3* db, const char* sql);
    ~SQLiteStatement();
    
    // Binding helpers
    bool bind_text(int index, const std::string& value);
    bool bind_int(int index, int value);
    bool bind_int64(int index, int64_t value);
    
    // Execution
    int step();
    void reset();
    
    // Column access
    int column_int(int index);
    int64_t column_int64(int index);
    std::string column_text(int index);
    
    // Access to raw statement
    sqlite3_stmt* get() { return stmt; }
    
    // Non-copyable
    SQLiteStatement(const SQLiteStatement&) = delete;
    SQLiteStatement& operator=(const SQLiteStatement&) = delete;
    
private:
    sqlite3_stmt* stmt;
};

// SQLite-based wallet for secure key storage and transaction management
class SQLiteWallet {
public:
    // Wallet metadata for crash recovery and reorg handling
    struct WalletMeta {
        int schema_version = 1;
        int last_applied_height = -1;
        std::string last_applied_hash = "";
        std::string pending_block_hash = "";
        int birth_height = 0;
        uint64_t created_at = 0;
        
        std::string toJSON() const;
        static WalletMeta fromJSON(const std::string& json);
    };
    
    // Key storage with optional encryption
    struct WalletKey {
        int id = 0;
        std::string pubkey;         // hex-encoded compressed pubkey
        std::string privkey;        // encrypted or plaintext WIF
        std::string enc_salt;       // encryption salt (if encrypted)
        std::string enc_nonce;      // encryption nonce (if encrypted)
        bool is_encrypted = false;
        uint64_t created_at = 0;
        
        std::string toJSON() const;
        static WalletKey fromJSON(const std::string& json);
    };
    
    // Address with script and key association
    struct WalletAddress {
        int id = 0;
        std::string address;        // bech32 address
        std::string script_pubkey;  // hex-encoded script
        std::string type;           // "p2wpkh", "p2wsh"
        int key_id = 0;            // 0 for watch-only
        bool watch_only = false;
        uint64_t created_at = 0;
        
        std::string toJSON() const;
        static WalletAddress fromJSON(const std::string& json);
    };
    
    // Transaction metadata
    struct WalletTx {
        int id = 0;
        std::string txid;
        std::string blockhash;
        int height = -1;
        uint64_t time = 0;
        std::string raw;            // hex-encoded raw transaction
        std::string direction;      // "recv", "send", "self"
        int64_t amount = 0;         // net amount (positive for recv)
        int64_t fee = 0;
        
        std::string toJSON() const;
        static WalletTx fromJSON(const std::string& json);
    };
    
    // UTXO tracking
    struct WalletUTXO {
        std::string txid;
        int vout = 0;
        int address_id = 0;
        int64_t value = 0;
        std::string script_pubkey;  // hex-encoded
        int height = 0;
        std::string spend_txid;     // empty if unspent
        int spend_height = -1;      // -1 if unspent
        
        std::string outpoint() const { return txid + ":" + std::to_string(vout); }
        bool isSpent() const { return !spend_txid.empty(); }
        
        std::string toJSON() const;
        static WalletUTXO fromJSON(const std::string& json);
    };
    
    // Block view for wallet application
    struct BlockView {
        std::string hash;
        int height = 0;
        uint64_t time = 0;
        std::vector<WalletTx> txs;
    };

public:
    SQLiteWallet();
    ~SQLiteWallet();
    
    // Initialization and configuration
    bool initialize(const std::string& wallet_path);
    void shutdown();
    bool isInitialized() const { return db != nullptr; }
    
    // Database integrity and diagnostics
    std::string integrity_check();
    
    // Wallet metadata operations
    WalletMeta getMeta();
    bool setMeta(const WalletMeta& meta);
    bool setMetaField(const std::string& field, const std::string& value);
    
    // Key management
    int generateNewKey();
    WalletKey getKey(int key_id);
    std::vector<WalletKey> getAllKeys();
    bool storeKey(const WalletKey& key);
    
    // Address management
    std::string getNewAddress();
    WalletAddress getAddress(const std::string& address);
    std::vector<WalletAddress> getAllAddresses();
    bool addWatchOnlyAddress(const std::string& address, const std::string& script_pubkey);
    bool isOurAddress(const std::string& address);
    
    // Transaction and UTXO management
    std::vector<WalletUTXO> listUnspent();
    std::vector<WalletUTXO> getUTXOsForAddress(const std::string& address);
    int64_t getBalance();
    int64_t getUnconfirmedBalance();
    
    // Block application (crash-safe)
    bool applyBlock(const BlockView& block);
    bool rollbackToHeight(int target_height);
    
    // Transaction operations
    bool upsertTx(const WalletTx& tx);
    WalletTx getTx(const std::string& txid);
    std::vector<WalletTx> getTransactions(int limit = 100);
    
    // UTXO operations
    bool insertUTXO(const WalletUTXO& utxo);
    bool markSpent(const std::string& txid, int vout, const std::string& spend_txid, int spend_height);
    bool isOurUTXO(const std::string& txid, int vout);
    
    // Backup and recovery
    bool backupToFile(const std::string& backup_path);
    bool backupToFileAPI(const std::string& backup_path);  // SQLite backup API
    bool restoreFromFile(const std::string& backup_path);
    bool validateIntegrity();
    
    // Connection info
    int getWriterSynchronousMode() const;
    
    // Reader connection management
    sqlite3* openReaderConnection() const;
    
    // Schema migration
    bool migrateSchema();
    
    // Database invariants checker
    bool checkInvariants() const;
    
    // Database performance statistics
    void logDatabaseStats() const;
    
    // HD Wallet functionality
    bool initializeHDWallet(const std::string& passphrase);
    bool unlockHDWallet(const std::string& passphrase);
    void lockHDWallet();
    bool isHDWalletUnlocked() const;
    std::string getNewHDAddress();
    std::string getNewChangeAddress();
    
    // Encryption (future)
    bool encryptWallet(const std::string& passphrase);
    bool unlockWallet(const std::string& passphrase, int timeout_seconds = 300);
    bool lockWallet();
    bool isLocked() const { return wallet_locked; }

private:
    sqlite3* db;
    std::string wallet_path;
    bool initialized;
    bool wallet_locked;
    mutable std::mutex wallet_mutex;
    
    // HD Wallet state
    bool hd_unlocked;
    uint8_t hd_seed[32];  // Encrypted seed in memory when unlocked
    std::string network_hrp;
    
    // Database setup
    bool createTables();
    bool upgradeSchema(int from_version, int to_version);
    bool setSQLitePragmas();
    
    // Helper methods
    bool execSQL(const std::string& sql);
    bool execSQL(const std::string& sql, const std::vector<std::string>& params);
    std::string queryString(const std::string& sql, const std::vector<std::string>& params = {});
    int queryInt(const std::string& sql, const std::vector<std::string>& params = {});
    std::vector<std::vector<std::string>> queryRows(const std::string& sql, const std::vector<std::string>& params = {});
    
    // Address derivation
    std::string deriveAddress(const std::string& pubkey);
    std::string deriveScriptPubKey(const std::string& pubkey);
    
    // Transaction analysis
    bool matchesOurAddress(const std::string& script_pubkey);
    int ensureAddressRow(const std::string& address, const std::string& script_pubkey, const std::string& type);
    
    // Crash recovery
    bool reapplyPendingBlock();
};

} // namespace Wallet
} // namespace Dinero
