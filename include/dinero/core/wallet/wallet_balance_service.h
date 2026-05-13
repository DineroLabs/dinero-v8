#pragma once

#include <string>
#include <vector>
#include <memory>
#include <sqlite3.h>

namespace dinero {

struct BalanceUTXO {
    std::string txid;
    int vout;
    uint64_t value;
    std::string scriptpubkey;
    int height;
    int confirmations;
    bool is_coinbase;
    bool is_spendable;
};

struct WalletBalance {
    uint64_t confirmed_spendable = 0;
    uint64_t unconfirmed = 0;
    uint64_t immature = 0;
    uint64_t total_spendable = 0;
};

class WalletBalanceService {
public:
    // CRITICAL FIX: Accept shared database connection instead of opening own
    WalletBalanceService(sqlite3* wallet_db, const std::string& explorer_db_path);
    ~WalletBalanceService();

    bool Initialize();
    void Shutdown();

    // Balance operations
    WalletBalance GetBalance(bool include_unconfirmed = false, bool include_immature = false);
    std::vector<BalanceUTXO> ListUnspent(int minconf = 1, int maxconf = 999999);
    
    // Address management
    bool AddWalletAddress(const std::string& address, const std::string& scriptpubkey_hex, 
                         const std::string& derivation_path = "", const std::string& purpose = "receive");
    std::vector<std::string> GetWalletAddresses();
    
    // Update operations (called by blockchain events)
    void OnBlockConnected(int height);
    void OnBlockDisconnected(int height);
    void OnMempoolTxAdded(const std::string& txid);
    void OnMempoolTxRemoved(const std::string& txid);

private:
    // CRITICAL FIX: Use shared database connection instead of own
    sqlite3* wallet_db_;  // Shared connection from SQLiteManager
    std::string explorer_db_path_;
    
    // Coinbase maturity (blocks required before coinbase UTXOs are spendable)
    static constexpr int COINBASE_MATURITY = 100;
    
    bool CreateTables();
    bool AttachExplorerDB();
    int GetCurrentTipHeight();
    
    // Helper methods
    std::string ScriptPubKeyToHex(const std::string& scriptpubkey);
    std::string HexToScriptPubKey(const std::string& hex);
};

} // namespace dinero
