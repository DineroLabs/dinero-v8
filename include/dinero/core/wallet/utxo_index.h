#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <sqlite3.h>

namespace dinero {

struct WalletUTXO {
    std::string txid;
    uint32_t vout;
    int64_t value;              // una
    std::vector<uint8_t> spk;   // scriptPubKey bytes
    std::string path;           // "m/84'/1448'/0'/0/12" etc
    int height;
    std::optional<int> spend_height; // nullopt = unspent
    
    UTXO() = default;
    UTXO(const std::string& txid_, uint32_t vout_, int64_t value_, 
         const std::vector<uint8_t>& spk_, const std::string& path_, int height_)
        : txid(txid_), vout(vout_), value(value_), spk(spk_), path(path_), height(height_) {}
};

class UTXOIndex {
public:
    explicit UTXOIndex(const std::string& db_path);
    ~UTXOIndex();
    
    // Database initialization
    bool Initialize();
    
    // UTXO management
    bool AddUTXO(const UTXO& utxo);
    bool SpendUTXO(const std::string& txid, uint32_t vout, int spend_height);
    bool IsUTXOSpent(const std::string& txid, uint32_t vout) const;
    
    // Query functions
    std::vector<UTXO> GetUnspentUTXOs() const;
    std::vector<UTXO> GetUTXOsForAddress(const std::string& address) const;
    std::optional<UTXO> GetUTXO(const std::string& txid, uint32_t vout) const;  // Get specific UTXO (optional style)
    bool GetUTXO(const std::string& txid, uint32_t vout, UTXO& utxo) const;     // Get specific UTXO (bool + ref style)
    int64_t GetBalance() const;
    int64_t GetBalanceForPath(const std::string& path_prefix) const;
    
    // Block processing
    void ProcessBlock(int height, const std::vector<std::string>& block_txs);
    void RevertBlock(int height);
    
    // Address recognition
    std::optional<std::string> IsOurScript(const std::vector<uint8_t>& scriptPubKey) const;
    
private:
    sqlite3* db_;
    std::string db_path_;
    
    // Helper functions
    bool CreateTables();
    bool PrepareStatements();
    void FinalizeStatements();
    
    // Prepared statements for performance
    sqlite3_stmt* stmt_add_utxo_;
    sqlite3_stmt* stmt_spend_utxo_;
    sqlite3_stmt* stmt_get_unspent_;
    sqlite3_stmt* stmt_get_balance_;
    sqlite3_stmt* stmt_is_spent_;
    sqlite3_stmt* stmt_get_utxo_;  // NEW: For GetUTXO(txid, vout)
};

// Transaction processing helpers
class TransactionProcessor {
public:
    static void ProcessTransaction(UTXOIndex& index, const std::string& txid, 
                                 const std::vector<std::pair<std::vector<uint8_t>, int64_t>>& outputs,
                                 const std::vector<std::pair<std::string, uint32_t>>& inputs,
                                 int height);
    
    static std::vector<uint8_t> ParseScriptPubKey(const std::string& hex);
    static bool IsP2WPKH(const std::vector<uint8_t>& script);
    static std::vector<uint8_t> ExtractPubKeyHash(const std::vector<uint8_t>& script);
};

} // namespace dinero
