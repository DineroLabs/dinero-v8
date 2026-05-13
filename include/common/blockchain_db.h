#ifndef DINERO_BLOCKCHAIN_DB_H
#define DINERO_BLOCKCHAIN_DB_H

#include <string>
#include <memory>
#include <mutex>

// RocksDB is disabled for Linux builds - this header is for macOS only
#if defined(HAVE_ROCKSDB) || defined(__APPLE__)
#if DIN_WITH_ROCKSDB
#include <rocksdb/db.h>
#endif
#endif

#include "compat/jsoncpp_compat.h"

#if !defined(HAVE_ROCKSDB) && !defined(__APPLE__)
// Stub for Linux builds without RocksDB
namespace Dinero { namespace Common {
class BlockchainDB {
public:
    BlockchainDB() {}
    ~BlockchainDB() {}
    bool initialize(const std::string&) { return false; }
    void shutdown() {}
    bool isInitialized() const { return false; }
};
}} // namespace Dinero::Common
#else

namespace Dinero {
namespace Common {

// Blockchain database manager for separate databases
class BlockchainDB {
private:
    // Single database with column families for true atomicity
    rocksdb::DB* db;
    std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
    
    // Column family handles (indices into cf_handles)
    rocksdb::ColumnFamilyHandle* cf_default;    // metadata: best_tip, state_hash, height
    rocksdb::ColumnFamilyHandle* cf_blocks;     // block data
    rocksdb::ColumnFamilyHandle* cf_chainstate; // UTXO set
    rocksdb::ColumnFamilyHandle* cf_index;      // block indices
    rocksdb::ColumnFamilyHandle* cf_undo;       // DEPRECATED: undo records (Phase 2 uses snapshots)
    rocksdb::ColumnFamilyHandle* cf_txstore;    // transaction store
    rocksdb::ColumnFamilyHandle* cf_wallet;     // wallet data
    
    // Single mutex for thread safety (atomic operations across CFs)
    mutable std::mutex db_mutex;
    
    // Helper methods
    std::string calculateScriptHash(const std::string& scriptHex);
    
    std::string base_path;
    
    bool initialized;
    
public:
    BlockchainDB();
    ~BlockchainDB();
    
    // Database initialization
    bool initialize(const std::string& path);
    void shutdown();
    void cleanupLockFiles();
    bool isInitialized() const { return initialized; }
    
    // Column family definitions for RocksDB
    enum ColumnFamily {
        CF_DEFAULT = 0,
        CF_BLOCKS = 1,
        CF_CHAINSTATE = 2,
        CF_ADDRESS_INDEX = 3,
        CF_TX_INDEX = 4,
        CF_SPENT_INDEX = 5,
        CF_SPK_INDEX = 6,
        CF_UNDO = 7,
        CF_METADATA = 8
    };
    
    // Enhanced UTXO structure for PSBT support
    struct UTXOData {
        std::string outpoint;           // "txid:vout"
        uint64_t amount;                // Amount in una
        std::string scriptPubKey;       // Hex-encoded script
        uint32_t height;                // Block height
        bool isCoinbase;                // Coinbase flag for maturity checks
        std::string address;            // Decoded address for indexing
        std::string addressType;        // "legacy", "p2wpkh", "p2sh"
        
        std::string toJSON() const;
        static UTXOData fromJSON(const std::string& json);
    };
    
    // Address index for fast UTXO lookup
    struct AddressIndex {
        std::string address;
        std::string scriptPubKeyHash;   // Hash160 of scriptPubKey
        std::vector<std::string> outpoints;
        
        std::string toJSON() const;
        static AddressIndex fromJSON(const std::string& json);
    };

    // ScriptPubKey index for fast UTXO lookup by script
    struct ScriptPubKeyIndex {
        std::string scriptPubKey;       // Hex-encoded script
        std::string scriptHash;         // Hash160 of scriptPubKey
        std::vector<std::string> outpoints;
        
        std::string toJSON() const;
        static ScriptPubKeyIndex fromJSON(const std::string& json);
    };
    
    // Chainstate (UTXO) operations
    bool storeUTXO(const std::string& outpoint, const std::string& utxo_data);
    std::string getUTXO(const std::string& outpoint);
    bool removeUTXO(const std::string& outpoint);
    bool hasUTXO(const std::string& outpoint);
    std::vector<std::string> getAllUTXOKeys();
    
    // Enhanced UTXO operations for PSBT
    bool storeUTXOData(const UTXOData& utxo);
    UTXOData getUTXOData(const std::string& outpoint);
    std::vector<UTXOData> getUTXOsForAddress(const std::string& address);
    std::vector<UTXOData> getUTXOsForScriptHash(const std::string& scriptHash);
    
    // Address indexing operations
    bool indexAddress(const std::string& address, const std::string& outpoint);
    bool removeAddressIndex(const std::string& address, const std::string& outpoint);
    std::vector<std::string> getOutpointsForAddress(const std::string& address);
    
    // ScriptPubKey indexing operations
    bool indexScriptPubKey(const std::string& scriptPubKey, const std::string& outpoint);
    bool removeScriptPubKeyIndex(const std::string& scriptPubKey, const std::string& outpoint);
    std::vector<std::string> getOutpointsForScriptPubKey(const std::string& scriptPubKey);
    std::vector<std::string> getOutpointsForScriptHash(const std::string& scriptHash);
    
    // Block operations
    bool storeBlock(uint32_t height, const std::string& block_data);
    bool storeBlockByHash(const std::string& block_hash, const std::string& block_data);
    std::string getBlock(uint32_t height);
    std::string getBlockByHash(const std::string& block_hash);
    bool hasBlock(uint32_t height);
    bool hasBlockByHash(const std::string& block_hash);
    
    // Index operations
    bool storeBlockIndex(uint32_t height, const std::string& index_data);
    bool storeBlockIndexByHash(const std::string& block_hash, const std::string& index_data);
    std::string getBlockIndex(uint32_t height);
    std::string getBlockIndexByHash(const std::string& block_hash);
    
    // Metadata operations for best block tracking
    bool storeMetadata(const std::string& key, const std::string& value);
    std::string getMetadata(const std::string& key);
    bool hasMetadata(const std::string& key);
    
    uint32_t getBestBlockHeight();
    std::string getBestBlockHash();
    
    // TxStore operations - append-only transaction storage
    bool storeTransaction(const std::string& txid, const std::string& raw_tx_hex);
    std::string getTransaction(const std::string& txid);
    bool hasTransaction(const std::string& txid);
    
    // TxMeta operations - transaction metadata for quick lookups
    struct TxMeta {
        std::string blockhash;
        uint32_t height;
        uint32_t index;  // Position in block
        
        std::string toJSON() const;
        static TxMeta fromJSON(const std::string& json);
    };
    
    bool storeTxMeta(const std::string& txid, const TxMeta& meta);
    TxMeta getTxMeta(const std::string& txid);
    bool hasTxMeta(const std::string& txid);
    
    // =========================================================================
    // DEPRECATED: Undo record for safe block disconnection
    // =========================================================================
    // Phase 2 uses snapshot-based rollback via ConsensusUTXOSet::Restore().
    // Undo records are no longer needed for reorg safety.
    // This will be removed in a future release.
    // =========================================================================
    struct [[deprecated("Phase 2 uses snapshot-restore, not undo records")]] UndoRecord {
        struct SpentOutput {
            std::string outpoint;       // "txid:vout"
            uint64_t amount;            // Amount in una
            std::string scriptPubKey;   // Hex-encoded script
            uint32_t height;            // Block height when created
            bool isCoinbase;            // Coinbase flag
            std::string address;        // Decoded address

            std::string toJSON() const;
            static SpentOutput fromJSON(const std::string& json);
        };

        std::string blockhash;
        uint32_t height;
        std::vector<SpentOutput> spentOutputs;

        std::string toJSON() const;
        static UndoRecord fromJSON(const std::string& json);
    };
    
    // Block commit result for atomic operations
    struct BlockCommitResult {
        bool success;
        std::string error;
        std::string blockhash;
        uint32_t height;
        size_t utxos_created;
        size_t utxos_spent;
    };
    
    // State checkpoint for recovery validation
    struct StateCheckpoint {
        uint32_t height;
        std::string blockhash;
        std::string state_hash;     // H(height || sample(UTXO) || cf_counts)
        uint64_t timestamp;
        size_t utxo_count;
        size_t block_count;
        
        std::string toJSON() const;
        static StateCheckpoint fromJSON(const std::string& json);
    };
    
    // Database health status
    struct DatabaseHealth {
        bool ok;
        uint32_t height;
        std::string state_hash;
        uint32_t recovered_blocks;
        std::string last_error;
        uint64_t last_checkpoint_time;
        
        std::string toJSON() const;
    };
    
    // =========================================================================
    // DEPRECATED: Undo operations
    // Phase 2 uses snapshot-restore via ConsensusUTXOSet.
    // These will be removed in a future release.
    // =========================================================================
    [[deprecated("Use ConsensusUTXOSet::Snapshot/Restore instead")]]
    bool storeUndoRecord(const std::string& blockhash, const UndoRecord& undo);
    [[deprecated("Use ConsensusUTXOSet::Snapshot/Restore instead")]]
    UndoRecord getUndoRecord(const std::string& blockhash);
    [[deprecated("Use ConsensusUTXOSet::Snapshot/Restore instead")]]
    bool hasUndoRecord(const std::string& blockhash);
    [[deprecated("Use ConsensusUTXOSet::Snapshot/Restore instead")]]
    bool removeUndoRecord(const std::string& blockhash);
    
    // Atomic block operations
    BlockCommitResult commitBlock(
        const std::string& blockhash,
        uint32_t height,
        const std::string& block_data,
        const std::string& index_data,
        const std::vector<UTXOData>& utxos_to_create,
        const std::vector<std::string>& utxos_to_spend,
        const UndoRecord& undo_record
    );
    
    // Recovery operations
    bool performStartupRecovery();
    bool rollbackToHeight(uint32_t target_height);
    bool validateChainIntegrity(uint32_t from_height = 0, uint32_t to_height = 0);
    
    // State checkpoint operations
    bool storeStateCheckpoint(const StateCheckpoint& checkpoint);
    StateCheckpoint getLatestStateCheckpoint();
    bool hasStateCheckpoint(uint32_t height);
    std::string calculateStateHash(uint32_t height);
    
    // Health monitoring
    DatabaseHealth getDatabaseHealth();
    bool validateStateConsistency();
    
    // Fault injection for testing (regtest only)
    void setFaultInjection(const std::string& fault_point);
    void triggerFaultInjection(const std::string& point);
    
    // Wallet operations
    bool storeWalletData(const std::string& key, const std::string& value);
    std::string getWalletData(const std::string& key);
    bool hasWalletData(const std::string& key);
    bool removeWalletData(const std::string& key);
    
    // Statistics and maintenance
    uint64_t getChainstateSize();
    uint64_t getBlocksSize();
    uint64_t getIndexSize();
    uint64_t getWalletSize();
    uint64_t getTotalSize();
    
    // Database maintenance
    bool compactChainstate();
    bool compactBlocks();
    bool compactIndex();
    bool compactWallet();
    bool compactAll();
    
    // Export functionality
    bool exportChainstateToJSON(const std::string& filename);
    bool exportBlocksToJSON(const std::string& filename);
    bool exportIndexToJSON(const std::string& filename);
    bool exportWalletToJSON(const std::string& filename);
    
    // Utility functions
    std::string getBasePath() const { return base_path; }
    std::string getChainstatePath() const { return base_path + "/chainstate"; }
    std::string getBlocksPath() const { return base_path + "/blocks"; }
    std::string getIndexPath() const { return base_path + "/index"; }
    std::string getWalletPath() const { return base_path + "/wallet"; }
};

} // namespace Common
} // namespace Dinero

#endif // !HAVE_ROCKSDB

#endif // DINERO_BLOCKCHAIN_DB_H 