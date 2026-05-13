#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif

namespace Dinero {
namespace MultiAccount {

/**
 * Account Types for different use cases
 */
enum class AccountType {
    PERSONAL,      // Personal spending account
    BUSINESS,      // Business transactions
    SAVINGS,       // Long-term savings
    INVESTMENT,    // Investment portfolio
    FAMILY,        // Family shared account
    CHARITY,       // Charitable donations
    CUSTOM         // User-defined custom account
};

/**
 * Account Settings and Configuration
 */
struct AccountSettings {
    std::string name;                    // Account display name
    std::string description;             // Account description
    AccountType type;                    // Account type
    uint32_t coinType;                   // Coin type for derivation
    uint32_t accountIndex;                // BIP44 account index
    bool isActive;                       // Whether account is active
    bool isHidden;                       // Whether account is hidden
    std::string color;                   // Account color for UI
    std::string icon;                    // Account icon
    Json::Value customSettings;          // Custom settings per account
    
    AccountSettings() 
        : name("Default Account")
        , description("Personal account")
        , type(AccountType::PERSONAL)
        , coinType(1448)
        , accountIndex(0)
        , isActive(true)
        , isHidden(false)
        , color("#3498db")
        , icon("👤")
    {}
};

/**
 * Account Information and State
 */
struct AccountInfo {
    std::string accountId;               // Unique account identifier
    AccountSettings settings;            // Account settings
    std::string currentAddress;          // Current receiving address
    uint32_t addressIndex;              // Current address index
    double balance;                      // Account balance
    std::vector<std::string> addresses; // Generated addresses
    std::vector<std::string> transactions; // Transaction history
    std::string mnemonic;               // Account mnemonic (encrypted)
    uint64_t createdAt;                 // Creation timestamp
    uint64_t lastUsed;                  // Last usage timestamp
    
    AccountInfo() 
        : accountId("")
        , addressIndex(0)
        , balance(0.0)
        , createdAt(0)
        , lastUsed(0)
    {}
};

/**
 * Multi-Account Manager
 * 
 * Manages multiple HD wallet accounts with different purposes and settings.
 * Each account has its own derivation path, settings, and transaction history.
 */
class MultiAccountManager {
public:
    explicit MultiAccountManager();
    ~MultiAccountManager();

    // Account Management
    std::string createAccount(const std::string& name, const std::string& description = "", 
                             const std::string& type = "PERSONAL", const std::string& color = "#3498db");
    bool deleteAccount(const std::string& accountId);
    bool restoreAccount(const std::string& mnemonic, const std::string& name, 
                       const std::string& description = "", const std::string& type = "PERSONAL");
    bool switchToAccount(const std::string& accountId);
    bool renameAccount(const std::string& accountId, const std::string& newName);
    bool updateAccountSettings(const std::string& accountId, const Json::Value& settings);
    
    // Account Information
    std::string getAccountName(const std::string& accountId);
    std::string getAccountDescription(const std::string& accountId);
    std::string getAccountType(const std::string& accountId);
    std::string getAccountColor(const std::string& accountId);
    std::string getAccountIcon(const std::string& accountId);
    double getAccountBalance(const std::string& accountId);
    std::string getCurrentAddress(const std::string& accountId);
    std::vector<std::string> getAccountAddresses(const std::string& accountId);
    std::vector<std::string> getAccountTransactions(const std::string& accountId);
    
    // Address Management
    std::string generateNewAddress(const std::string& accountId);
    std::string generateAddressAt(const std::string& accountId, uint32_t index);
    bool validateAddress(const std::string& address);
    
    // Transaction Management
    bool sendTransaction(const std::string& accountId, const std::string& toAddress, 
                                   double amount, const std::string& memo = "");
    std::vector<std::string> getTransactionHistory(const std::string& accountId);
    bool importTransaction(const std::string& accountId, const std::string& txId, 
                                     const std::string& type, double amount, const std::string& address);
    
    // Backup and Recovery
    std::string exportAccount(const std::string& accountId);
    std::string exportAllAccounts();
    bool importAccount(const std::string& accountData);
    bool importAllAccounts(const std::string& accountsData);
    std::string getAccountMnemonic(const std::string& accountId);
    
    // Settings and Configuration
    Json::Value getAccountSettings(const std::string& accountId);
    bool setAccountSettings(const std::string& accountId, const Json::Value& settings);
    std::vector<std::string> getAccountTypes();
    std::vector<std::string> getAccountColors();
    std::vector<std::string> getAccountIcons();
    
    // Utility Functions
    std::vector<std::string> getAllAccountIds();
    std::vector<std::string> getActiveAccountIds();
    std::vector<std::string> getHiddenAccountIds();
    bool isAccountActive(const std::string& accountId);
    bool isAccountHidden(const std::string& accountId);
    int getAccountCount();
    double getTotalBalance();
    
    // Getters
    std::string currentAccountId() const { return currentAccountId_; }
    std::vector<std::string> accountIds() const;
    int accountCount() const { return accounts_.size(); }

/** signals removed for daemon-only **/
    void currentAccountChanged(const std::string& accountId);
    void accountsChanged();
    void accountCreated(const std::string& accountId);
    void accountDeleted(const std::string& accountId);
    void accountSwitched(const std::string& accountId);
    void accountUpdated(const std::string& accountId);
    void balanceUpdated(const std::string& accountId, double balance);
    void transactionAdded(const std::string& accountId, const std::string& txId);
    void addressGenerated(const std::string& accountId, const std::string& address);

/** private slots removed for daemon-only **/
    void onBalanceUpdate();
    void onTransactionUpdate();

private:
    // Core Data
    std::map<std::string, AccountInfo> accounts_;
    std::string currentAccountId_;
    std::string dataDir_;
    std::mutex accountsMutex_;
    
    // HD Wallet Integration
    // Note: HDWallet is included via wallet/hd_wallet.h
    
    // Helper Methods
    std::string generateAccountId();
    std::string getAccountFilePath(const std::string& accountId);
    bool loadAccount(const std::string& accountId);
    bool saveAccount(const std::string& accountId);
    bool loadAllAccounts();
    bool saveAllAccounts();
    AccountType stringToAccountType(const std::string& type);
    std::string accountTypeToString(AccountType type);
    std::string encryptMnemonic(const std::string& mnemonic);
    std::string decryptMnemonic(const std::string& encryptedMnemonic);
    bool validateAccountId(const std::string& accountId);
    void updateAccountLastUsed(const std::string& accountId);
    void initializeDefaultAccount();
};

/**
 * Account Factory
 * 
 * Creates and manages account instances with different configurations
 */
class AccountFactory {
public:
    static std::unique_ptr<AccountInfo> createAccount(
        const std::string& name,
        const std::string& description,
        AccountType type,
        const std::string& color = "#3498db",
        const std::string& icon = "👤"
    );
    
    static AccountSettings createDefaultSettings(
        const std::string& name,
        AccountType type,
        const std::string& color = "#3498db"
    );
    
    static std::string generateAccountId();
    static std::string getDefaultAccountName(AccountType type);
    static std::string getDefaultAccountColor(AccountType type);
    static std::string getDefaultAccountIcon(AccountType type);
};

/**
 * Account Validator
 * 
 * Validates account data and operations
 */
class AccountValidator {
public:
    static bool validateAccountName(const std::string& name);
    static bool validateAccountDescription(const std::string& description);
    static bool validateAccountColor(const std::string& color);
    static bool validateAccountIcon(const std::string& icon);
    static bool validateAccountSettings(const Json::Value& settings);
    static bool validateAccountId(const std::string& accountId);
    static bool validateMnemonic(const std::string& mnemonic);
    static bool validateDerivationPath(uint32_t coinType, uint32_t accountIndex);
};

/**
 * Account Backup Manager
 * 
 * Handles backup and recovery of account data
 */
class AccountBackupManager {
public:
    static std::string createAccountBackup(const AccountInfo& account);
    static std::string createAllAccountsBackup(const std::map<std::string, AccountInfo>& accounts);
    static bool restoreAccountFromBackup(const std::string& backupData, AccountInfo& account);
    static bool restoreAllAccountsFromBackup(const std::string& backupData, 
                                           std::map<std::string, AccountInfo>& accounts);
    static bool validateBackupData(const std::string& backupData);
    static std::string encryptBackup(const std::string& backupData, const std::string& password);
    static std::string decryptBackup(const std::string& encryptedBackup, const std::string& password);
};

} // namespace MultiAccount
} // namespace Dinero
