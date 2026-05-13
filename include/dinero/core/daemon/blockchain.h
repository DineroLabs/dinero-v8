#pragma once

#include <string>
#include <memory>
#include <vector>
#include "dinero/core/database/sqlite_manager.h"

// Forward declarations
namespace dinero {
    struct Block;  // defined as struct in primitives/block.h
    struct Transaction;
}

namespace din::sp {
    class ScannerManager;
}

namespace dinero {

// Transaction creation result structure
struct TransactionCreationResult {
    bool success = false;
    std::string txid = "";
    uint64_t fee_amount = 0;
    std::string error_message = "";
    std::string transaction = ""; // Add missing transaction field
};

class Blockchain {
public:
    explicit Blockchain(const std::string& datadir);
    ~Blockchain();

    // Genesis block management
    bool initializeGenesisBlock();
    
    // Cleanup and shutdown
    void shutdown();
    
    // Blockchain state queries
    uint32_t getBlockHeight() const;
    std::string getBestBlockHash() const;
    
    // Legacy method names for compatibility
    uint32_t getLatestHeight() const { return getBlockHeight(); }
    std::string getLatestHash() const { return getBestBlockHash(); }
    uint32_t getHeight() const { return getBlockHeight(); }
    
    // Database access
    const SQLiteManager* getDatabaseManager() const { return db_manager_.get(); }
    
    // Database access for compatibility (these will be implemented properly)
    const SQLiteManager* getDatabase() const { return db_manager_.get(); }
    
    // REMOVED: Direct blockchain database access
    // Blockchain data is now in ChainDB (RocksDB), not SQLite
    // sqlite3* getBlockchainDatabase() const { return nullptr; }
    
    // Additional methods needed by RPC system (will be implemented properly)
    bool hasBlock(uint32_t height) const;
    bool hasBlockByHash(const std::string& hash) const;
    std::string getBlock(uint32_t height) const;
    std::string getBlockByHash(const std::string& hash) const;
    std::string getTransaction(const std::string& txid) const;
    std::string getUTXO(const std::string& outpoint) const;
    uint32_t getBestBlockHeight() const { return getBlockHeight(); }
    
    // Additional methods needed by RPC system (will be implemented properly)
    bool isCoinbaseMature(uint32_t utxoHeight, uint32_t tipHeight) const;
    bool isValidAddress(const std::string& address) const;
    std::string getScriptPubKey(const std::string& address) const;
    bool importWatchXPub(const std::string& xpub);
    bool importWatchAddr(const std::string& address);
    
    // Additional methods needed by RPC system
    TransactionCreationResult createRawTransaction(const std::vector<std::pair<std::string, uint32_t>>& inputs, 
                                                 const std::vector<std::pair<std::string, uint64_t>>& outputs);
    std::vector<std::pair<std::string, uint64_t>> getUTXOsForAddress(const std::string& address);
    std::string getWatchBalances() const;

    // Block validation and management methods
    bool validateBlock(const Block& block) const;
    bool addBlock(const Block& block);
    std::vector<uint8_t> calculateBlockHash(const Block& block) const;
    bool validateTransaction(const Transaction& tx) const;
    
    // Mining-related methods
    std::string createScriptPubKeyFromAddress(const std::string& address) const;
    std::string createCoinbaseScript(uint32_t height, const std::string& message) const;
    uint64_t calculateBlockReward(uint32_t height) const;
    std::string computeMerkleRoot(const std::vector<Transaction>& transactions) const;
    std::string computeBlockHash(const Block& block) const;

private:
    std::string datadir_;
    std::unique_ptr<SQLiteManager> db_manager_;
    
    // Silent Payments scanner
    std::unique_ptr<din::sp::ScannerManager> sp_scanner_manager_;
    
    // Genesis block operations
    bool storeGenesisBlock(const void* genesis);
    
    // Database operations
    bool executeSQL(void* db, const std::string& sql);
    
    // Utility functions
    std::string bytesToHex(const uint8_t* bytes, size_t size) const;
};

} // namespace dinero 