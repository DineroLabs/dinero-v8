#pragma once

#include <string>
#include <vector>
#include <memory>
#include <sqlite3.h>
#include <cstdint>

namespace dinero {

// Forward declarations
struct WalletDescriptor;
struct WalletAddress;
struct WalletTransaction;
struct WalletUTXO;

/**
 * SQLite-based wallet implementation
 * 
 * Architecture:
 * - RocksDB: Chain/UTXO/state (keep existing)
 * - SQLite: Wallet data (descriptors, addresses, keys, transactions)
 * 
 * Benefits:
 * - Easy migrations and schema evolution
 * - Simple backups (single file)
 * - Great tooling and debugging
 * - Perfect for relational wallet data
 */
class SQLiteWallet {
public:
    SQLiteWallet();
    ~SQLiteWallet();

    // Database management
    bool open(const std::string& wallet_path);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // Wallet initialization
    bool initializeWallet(const std::string& wallet_name);
    bool createTables();
    bool setSchemaVersion(int version);
    int getSchemaVersion();

    // Descriptor management
    bool addDescriptor(const std::string& descriptor, bool active = true, bool internal = false);
    std::vector<WalletDescriptor> getDescriptors();
    bool updateDescriptorState(int desc_id, int next_index, int lookahead = 20);

    // Address management
    bool addAddress(int desc_id, int idx, const std::string& address, 
                   const std::vector<uint8_t>& scriptpubkey, const std::string& label = "");
    std::vector<WalletAddress> getAddresses(int desc_id, int start_idx = 0, int count = 20);
    bool setAddressLabel(const std::string& address, const std::string& label);

    // Key management
    bool addKey(int desc_id, int idx, const std::vector<uint8_t>& pubkey, 
                const std::vector<uint8_t>& privkey_enc = {});
    std::vector<uint8_t> getPrivateKey(int desc_id, int idx);

    // Transaction management
    bool addTransaction(const std::vector<uint8_t>& txid, const std::vector<uint8_t>& rawtx,
                       int height = -1, int64_t received_sats = 0, int64_t sent_sats = 0);
    std::vector<WalletTransaction> getTransactions(int limit = 100, int offset = 0);

    // UTXO management
    bool addUTXO(const std::vector<uint8_t>& outpoint, int64_t value_sats,
                 const std::vector<uint8_t>& scriptpubkey, int desc_id, int idx, int height);
    std::vector<WalletUTXO> getUTXOs(int desc_id = -1);

    // Backup and recovery
    bool backup(const std::string& backup_path);
    bool restore(const std::string& backup_path);

private:
    sqlite3* m_db;
    std::string m_wallet_path;
    
    // Helper methods
    bool executeSQL(const std::string& sql);
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    
    // Schema management
    bool createMetaTable();
    bool createDescriptorsTable();
    bool createDescriptorStateTable();
    bool createKeysTable();
    bool createAddressesTable();
    bool createTransactionsTable();
    bool createUTXOsTable();
    bool createIndexes();
};

// Data structures
struct WalletDescriptor {
    int id;
    std::string descriptor;
    bool active;
    bool internal;
};

struct WalletAddress {
    int id;
    int desc_id;
    int idx;
    std::string address;
    std::vector<uint8_t> scriptpubkey;
    std::string label;
    int64_t created_at;
};

struct WalletTransaction {
    std::vector<uint8_t> txid;
    std::vector<uint8_t> rawtx;
    int height;
    int64_t received_sats;
    int64_t sent_sats;
    int64_t timestamp;
};

struct WalletUTXO {
    std::vector<uint8_t> outpoint;
    int64_t value_sats;
    std::vector<uint8_t> scriptpubkey;
    int desc_id;
    int idx;
    int height;
};

} // namespace dinero
