#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

struct sqlite3; // forward decl

namespace dinero {

struct AddressRow {
    std::string address;
    std::optional<std::string> label;
    int account = 0;
    int change  = 0;
    int index   = 0;
    bool external = false;  // true for address book entries, false for HD addresses
};

class WalletManager {
public:
    explicit WalletManager(const std::filesystem::path& dataDir);
    ~WalletManager();

    std::vector<std::string> listWallets() const;
    bool exists(const std::string& name) const;

    void create(const std::string& name);
    void open(const std::string& name);
    void rename(const std::string& oldName, const std::string& newName);
    void remove(const std::string& name); // refuses if current or has UTXOs

    std::string current() const { return current_; }
    bool hasActiveWallet() const { return !current_.empty(); }
    std::string getCurrentWalletName() const { return current_; }

    // Labels and Address Book
    void setAddressLabel(const std::string& addr, const std::string& label);
    std::optional<std::string> getAddressLabel(const std::string& addr) const;
    std::vector<AddressRow> listAddresses(bool includeLabels = true) const;
    void removeAddress(const std::string& addr);  // removes from address book only
    
    // HD Wallet Address Management
    void addHDAddress(const std::string& addr, int account, int change, int index, const std::string& label = "");
    int getNextAddressIndex(int account = 0, int change = 0) const;
    bool isAddressMine(const std::string& addr) const;
    bool isScriptMine(const std::string& script_pubkey) const;
    
    // Address generation
    std::string getNewAddress(const std::string& label = "");
    std::string getNewChangeAddress(const std::string& label = "");
    
    // Database access for RPC handlers
    sqlite3* getCurrentDatabase() const;
    
    // Wallet encryption/decryption
    void encryptWallet(const std::string& passphrase);
    void decryptWallet(const std::string& passphrase);
    void changePassphrase(const std::string& oldPassphrase, const std::string& newPassphrase);
    void lockWallet();
    void unlockWallet(const std::string& passphrase, int timeoutSeconds = 0);
    bool isWalletEncrypted() const;
    bool isWalletLocked() const;
    
    // Balance calculation from database
    struct Balance {
        double confirmed = 0.0;          // Confirmed and spendable (includes mature coinbase)
        double unconfirmed = 0.0;        // Unconfirmed (in mempool)
        double immature = 0.0;           // Immature coinbase (not yet spendable)
        double total = 0.0;              // Total balance (confirmed + unconfirmed + immature)
        double spendable = 0.0;          // Actually spendable (confirmed only)
        int utxo_count = 0;
        int immature_utxo_count = 0;
    };
    Balance getBalance() const;
    Balance getAddressBalance(const std::string& address) const;
    
    // Transaction history queries
    struct TransactionInfo {
        std::string txid;
        std::string address;
        double amount;
        int confirmations;
        std::string category; // "send", "receive", "generate"
        int64_t time;
        std::string label;
        bool is_coinbase;
    };
    std::vector<TransactionInfo> getTransactionHistory(int limit = 100, int offset = 0) const;
    std::vector<TransactionInfo> getAddressHistory(const std::string& address, int limit = 100) const;
    
    // UTXO queries for PSBT creation and spending
    struct WalletUTXO {
        std::string txid;
        uint32_t vout;
        uint64_t amount_una;
        double amount_din;
        std::string address;
        int confirmations;
        uint32_t height;        // Block height where this UTXO was created
        bool spendable;         // Considers both confirmations and coinbase maturity
        bool is_coinbase;
        bool is_mature;         // True if coinbase is mature (or not coinbase)
        std::string label;
        
        // Additional fields for spending
        std::string script_pubkey;
        bool is_spent;
        
        // Convenience getters for spend RPC handlers
        int64_t getAmount() const { return static_cast<int64_t>(amount_una); }
        int getVout() const { return static_cast<int>(vout); }
    };
    std::vector<UTXO> listUnspentUTXOs(int min_confirmations = 1, int max_confirmations = 9999999) const;
    std::vector<UTXO> getUTXOsForAddress(const std::string& address, int min_confirmations = 1) const;
    
    // Blockchain height access for maturity calculations
    void setBlockchainHeight(uint32_t height) { 
        current_blockchain_height_ = height; 
        // Update UTXO maturity when blockchain height changes
        updateUTXOMaturity();
    }
    void loadBlockchainHeight(); // Load height from tip table
    void runHealthCheck(); // Run PRAGMA quick_check at startup
    std::string runIntegrityCheck(); // Run PRAGMA integrity_check for admin RPC
    uint32_t getBlocksUntilMature(uint32_t utxo_height) const; // Calculate blocks until UTXO matures
    void validateSchemaVersion(); // Validate schema version matches compiled version
    void checkFilePermissions(); // Check database file and directory permissions
    uint32_t getCurrentBlockchainHeight() const { return current_blockchain_height_; }
    
    // Address management for transaction scanning
    std::vector<std::string> getWalletAddresses() const;
    
    // Wallet rescan functionality
    bool rescanBlockchain(int start_height = 0, int gap_limit = 20);
    
    // Transaction recording for real-time updates
    bool addTransaction(const std::string& txid, const std::string& address, double amount,
                       const std::string& category, bool is_coinbase = false, 
                       const std::string& label = "", int64_t time = 0, uint32_t height = 0);
    bool confirmTransaction(const std::string& txid, uint32_t height);
    
    // Settings management for mining address persistence
    void setSetting(const std::string& key, const std::string& value, const std::string& wallet = "", const std::string& network = "");
    std::string getSetting(const std::string& key, const std::string& wallet = "", const std::string& network = "") const;
    bool hasSetting(const std::string& key, const std::string& wallet = "", const std::string& network = "") const;
    void setMiningAddress(const std::string& address, const std::string& wallet, const std::string& network);
    std::string getMiningAddress(const std::string& wallet = "", const std::string& network = "") const;
    
    // UTXO Management for spending (using existing UTXO struct above)
    
    std::vector<UTXO> getAvailableUTXOs() const;
    bool addUTXO(const std::string& txid, int vout, int64_t amount, 
                 const std::string& address, const std::string& script_pubkey,
                 int height, bool is_coinbase);
    bool spendUTXO(const std::string& txid, int vout);
    void updateUTXOMaturity();

private:
    std::filesystem::path dataDir_;
    std::string current_;
    int current_wallet_id_ = -1;
    sqlite3* db_ = nullptr;
    
    // Encryption state
    bool wallet_encrypted_ = false;
    bool wallet_locked_ = true;
    std::string encryption_key_;
    int64_t unlock_timeout_ = 0;
    int64_t unlock_time_ = 0;
    
    // Blockchain state for maturity calculations
    mutable uint32_t current_blockchain_height_ = 0;

    void initializeDatabase();
    void close();
    static std::string sanitize(const std::string& name);
    int getWalletId(const std::string& name) const;
    
    // Transaction analysis for blockchain scanning
    bool analyzeTransaction(const char* raw_hex, const std::vector<std::string>& wallet_addresses, 
                          TransactionInfo& tx, uint32_t height) const;
    double calculateMiningReward(uint32_t height) const;
    void setCurrentWallet(const std::string& name, int wallet_id);
    
    // Address validation
    bool isValidDineroBech32(const std::string& addr) const;
    
    // Encryption helpers
    std::string deriveKey(const std::string& passphrase, const std::string& salt) const;
    std::string encryptData(const std::string& data, const std::string& key) const;
    std::string decryptData(const std::string& encryptedData, const std::string& key) const;
    void checkUnlockTimeout();

    // SQLite helpers/migration
    static void exec(sqlite3* db, const char* sql);
    static int  getUserVersion(sqlite3* db);
    static void setUserVersion(sqlite3* db, int v);
    static bool tableExists(sqlite3* db, const char* name);
    static bool columnExists(sqlite3* db, const char* table, const char* col);
    static void migrate(sqlite3* db);
};

} // namespace dinero
