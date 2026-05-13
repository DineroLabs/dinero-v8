#pragma once

#include "storage/storage_interface.h"
#include "storage/rocksdb_config.h" // Phase 6A: Advanced configuration
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/iterator.h>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/statistics.h>
#include <memory>
#include <mutex>

namespace dinero {
namespace storage {

/**
 * RocksDB write batch implementation
 */
class RocksDBWriteBatch : public WriteBatch {
public:
    explicit RocksDBWriteBatch();
    ~RocksDBWriteBatch() override = default;
    
    // WriteBatch interface
    void putBlock(const std::string& hash, const Block& block) override;
    void deleteBlock(const std::string& hash) override;
    void putTransaction(const std::string& hash, const Transaction& tx) override;
    void deleteTransaction(const std::string& hash) override;
    void putUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) override;
    void deleteUTXO(const std::string& outpoint) override;
    void putChainState(const std::string& key, const std::vector<uint8_t>& value) override;
    void deleteChainState(const std::string& key) override;
    void putMetadata(const std::string& key, const std::vector<uint8_t>& value) override;
    void deleteMetadata(const std::string& key) override;
    StorageResult commit() override;
    StorageResult commitSync(bool sync) override;
    void clear() override;
    size_t size() const override;
    
    // Internal access for RocksDBBackend
    rocksdb::WriteBatch& getBatch() { return batch_; }
    void setDB(rocksdb::DB* db) { db_ = db; }

private:
    rocksdb::WriteBatch batch_;
    rocksdb::DB* db_;
    
    std::string makeBlockKey(const std::string& hash) const;
    std::string makeTransactionKey(const std::string& hash) const;
    std::string makeUTXOKey(const std::string& outpoint) const;
    std::string makeChainStateKey(const std::string& key) const;
    std::string makeMetadataKey(const std::string& key) const;
};

/**
 * RocksDB iterator implementation
 */
class RocksDBIterator : public StorageIterator {
public:
    explicit RocksDBIterator(std::unique_ptr<rocksdb::Iterator> it);
    ~RocksDBIterator() override = default;
    
    // StorageIterator interface
    bool isValid() const override;
    void seekToFirst() override;
    void seekToLast() override;
    void seek(const std::string& key) override;
    void next() override;
    void prev() override;
    std::string key() const override;
    std::vector<uint8_t> value() const override;
    StorageResult status() const override;

private:
    std::unique_ptr<rocksdb::Iterator> iterator_;
};

/**
 * RocksDB storage backend implementation
 * 
 * High-performance storage backend using RocksDB:
 * - Optimized for blockchain workloads
 * - Compression and bloom filters
 * - Column families for different data types
 * - Atomic batch operations
 * - Background compaction
 */
class RocksDBBackend : public StorageInterface {
public:
    RocksDBBackend();
    explicit RocksDBBackend(const RocksDBConfig& config); // Phase 6A: Custom config
    ~RocksDBBackend() override;

    // StorageInterface implementation
    StorageResult initialize(const std::string& data_dir) override;
    StorageResult shutdown() override;
    bool isHealthy() const override;

    // Phase 6A: Configuration access
    const RocksDBConfig& getConfig() const { return config_; }
    void setConfig(const RocksDBConfig& config);
    std::string getConfigSummary() const;
    
    // Block operations
    StorageResult putBlock(const std::string& hash, const Block& block) override;
    StorageResult getBlock(const std::string& hash, Block& block) const override;
    bool hasBlock(const std::string& hash) const override;
    StorageResult deleteBlock(const std::string& hash) override;
    StorageResult getBlockHeader(const std::string& hash, BlockHeader& header) const override;
    
    // Transaction operations
    StorageResult putTransaction(const std::string& hash, const Transaction& tx) override;
    StorageResult getTransaction(const std::string& hash, Transaction& tx) const override;
    bool hasTransaction(const std::string& hash) const override;
    StorageResult deleteTransaction(const std::string& hash) override;
    
    // UTXO operations
    StorageResult putUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) override;
    StorageResult getUTXO(const std::string& outpoint, std::vector<uint8_t>& utxo_data) const override;
    bool hasUTXO(const std::string& outpoint) const override;
    StorageResult deleteUTXO(const std::string& outpoint) override;
    StorageResult getUTXOsForAddress(const std::string& address, 
                                   std::vector<std::pair<std::string, std::vector<uint8_t>>>& utxos) const override;
    
    // Chain state operations
    StorageResult putChainState(const std::string& key, const std::vector<uint8_t>& value) override;
    StorageResult getChainState(const std::string& key, std::vector<uint8_t>& value) const override;
    StorageResult deleteChainState(const std::string& key) override;
    
    // Metadata operations
    StorageResult putMetadata(const std::string& key, const std::vector<uint8_t>& value) override;
    StorageResult getMetadata(const std::string& key, std::vector<uint8_t>& value) const override;
    StorageResult deleteMetadata(const std::string& key) override;
    
    // Batch operations
    std::unique_ptr<WriteBatch> createWriteBatch() override;
    StorageResult writeBatch(WriteBatch& batch) override;
    
    // Iteration
    std::unique_ptr<StorageIterator> createIterator(const std::string& prefix = "") override;
    
    // Maintenance
    StorageResult compact() override;
    StorageResult verify() override;
    StorageStats getStats() const override;
    StorageResult backup(const std::string& backup_dir) override;
    StorageResult restore(const std::string& backup_dir) override;
    
    // Configuration
    StorageResult setOption(const std::string& key, const std::string& value) override;
    std::string getOption(const std::string& key) const override;
    
    // Size estimation
    uint64_t getApproximateSize(const std::string& start_key, const std::string& end_key) const override;

private:
    // Column family handles
    enum class ColumnFamily {
        DEFAULT = 0,
        BLOCKS,
        TRANSACTIONS,
        UTXOS,
        CHAIN_STATE,
        METADATA,
        COUNT
    };
    
    // RocksDB instances with column family handles
    std::unique_ptr<rocksdb::DB> db_;
    std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;

    // Phase 6A: High-level configuration
    RocksDBConfig config_;

    // Configuration
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;

    // Statistics and monitoring
    std::shared_ptr<rocksdb::Statistics> statistics_;
    std::shared_ptr<rocksdb::Cache> block_cache_;

    // Thread safety
    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    
    // Internal helpers
    bool init(const std::string& path);
    void close();
    void configureOptions();
    StorageResult initializeColumnFamilies(const std::string& data_dir);
    rocksdb::ColumnFamilyHandle* getColumnFamily(ColumnFamily cf) const;
    std::string makeKey(const std::string& prefix, const std::string& key) const;
    StorageResult convertStatus(const rocksdb::Status& status) const;
    
    // Storage interface requirements
    bool selfTest() override { return isHealthy(); }
    std::string name() const override { return "RocksDB"; }
    
    // Serialization helpers
    std::vector<uint8_t> serializeBlock(const Block& block) const;
    bool deserializeBlock(const std::vector<uint8_t>& data, Block& block) const;
    std::vector<uint8_t> serializeTransaction(const Transaction& tx) const;
    bool deserializeTransaction(const std::vector<uint8_t>& data, Transaction& tx) const;
    std::vector<uint8_t> serializeBlockHeader(const BlockHeader& header) const;
    bool deserializeBlockHeader(const std::vector<uint8_t>& data, BlockHeader& header) const;
    
    public:
    // Key prefixes for different data types
    static constexpr const char* BLOCK_PREFIX = "blk:";
    static constexpr const char* TRANSACTION_PREFIX = "tx:";
    static constexpr const char* UTXO_PREFIX = "utxo:";
    static constexpr const char* CHAIN_STATE_PREFIX = "cs:";
    static constexpr const char* METADATA_PREFIX = "meta:";

private:
    static constexpr const char* BLOCK_HEADER_PREFIX = "hdr:";
    static constexpr const char* ADDRESS_UTXO_PREFIX = "addr:";
};

} // namespace storage
} // namespace dinero
