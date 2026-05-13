#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <cstdint>
#include "storage/storage_config.h"

// Forward declarations
namespace dinero {
    struct Block;
    struct BlockHeader;
    struct Transaction;
    struct ChainState;
}

namespace dinero {
namespace storage {

/**
 * Storage operation result
 */
enum class StorageResult {
    SUCCESS,
    NOT_FOUND,
    ALREADY_EXISTS,
    CORRUPTION,
    IO_ERROR,
    INVALID_DATA,
    INSUFFICIENT_SPACE,
    PERMISSION_DENIED,
    INVALID_OPERATION,
    RESOURCE_EXHAUSTED
};

/**
 * Storage statistics for monitoring
 */
struct StorageStats {
    uint64_t total_size_bytes;
    uint64_t used_size_bytes;
    uint64_t free_size_bytes;
    uint64_t block_count;
    uint64_t transaction_count;
    uint64_t utxo_count;
    double compression_ratio;
    std::string backend_name;
    std::string backend_type;
    std::string version;
    
    // Extended statistics for RPC
    std::string data_directory;
    bool is_healthy = true;
    uint64_t uptime_seconds = 0;
    
    // Size breakdown
    uint64_t data_size_bytes = 0;
    uint64_t index_size_bytes = 0;
    uint64_t log_size_bytes = 0;
    
    // Operation counts
    uint64_t total_reads = 0;
    uint64_t total_writes = 0;
    uint64_t total_deletes = 0;
    uint64_t failed_operations = 0;
    
    // Performance metrics
    uint64_t avg_read_latency_us = 0;
    uint64_t avg_write_latency_us = 0;
    uint64_t p95_read_latency_us = 0;
    uint64_t p95_write_latency_us = 0;
    
    // Resource usage
    uint64_t memory_usage_bytes = 0;
    int open_file_descriptors = 0;
    uint64_t compaction_debt_bytes = 0;
    
    // Backend-specific properties
    std::unordered_map<std::string, std::string> rocksdb_properties;
    std::string leveldb_stats;
};

/**
 * Batch write operation for atomic updates
 */
class WriteBatch {
public:
    virtual ~WriteBatch() = default;
    
    // Block operations
    virtual void putBlock(const std::string& hash, const Block& block) = 0;
    virtual void deleteBlock(const std::string& hash) = 0;
    
    // Transaction operations
    virtual void putTransaction(const std::string& hash, const Transaction& tx) = 0;
    virtual void deleteTransaction(const std::string& hash) = 0;
    
    // UTXO operations
    virtual void putUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) = 0;
    virtual void deleteUTXO(const std::string& outpoint) = 0;
    
    // Chain state operations
    virtual void putChainState(const std::string& key, const std::vector<uint8_t>& value) = 0;
    virtual void deleteChainState(const std::string& key) = 0;
    
    // Metadata operations
    virtual void putMetadata(const std::string& key, const std::vector<uint8_t>& value) = 0;
    virtual void deleteMetadata(const std::string& key) = 0;
    /**
     * Commit the batch atomically (async)
     */
    virtual StorageResult commit() = 0;
    /**
     * Commit the batch atomically with sync control
     * @param sync If true, forces fsync for durability (required for tip commits)
     */
    virtual StorageResult commitSync(bool sync) = 0;
    // Clear all operations
    virtual void clear() = 0;
    
    // Get operation count
    virtual size_t size() const = 0;
};

/**
 * Storage iterator for range queries
 */
class StorageIterator {
public:
    virtual ~StorageIterator() = default;
    
    // Iterator control
    virtual bool isValid() const = 0;
    virtual void seekToFirst() = 0;
    virtual void seekToLast() = 0;
    virtual void seek(const std::string& key) = 0;
    virtual void next() = 0;
    virtual void prev() = 0;
    
    // Data access
    virtual std::string key() const = 0;
    virtual std::vector<uint8_t> value() const = 0;
    
    // Status
    virtual StorageResult status() const = 0;
};

/**
 * Abstract storage interface for blockchain data
 * 
 * Provides a unified interface for different storage backends:
 * - RocksDB for production (high performance, compression)
 * - SQLite for development/testing (simple, portable)
 * - Memory for unit tests (fast, ephemeral)
 */
class StorageInterface {
public:
    virtual ~StorageInterface() = default;
    
    /**
     * Initialize storage backend
     */
    virtual StorageResult initialize(const std::string& data_dir) = 0;
    
    /**
     * Shutdown storage backend
     */
    virtual StorageResult shutdown() = 0;
    
    /**
     * Check if storage is healthy
     */
    virtual bool isHealthy() const = 0;
    
    // === Block Storage ===
    
    /**
     * Store a block
     */
    virtual StorageResult putBlock(const std::string& hash, const Block& block) = 0;
    
    /**
     * Retrieve a block by hash
     */
    virtual StorageResult getBlock(const std::string& hash, Block& block) const = 0;
    
    /**
     * Check if block exists
     */
    virtual bool hasBlock(const std::string& hash) const = 0;
    
    /**
     * Delete a block
     */
    virtual StorageResult deleteBlock(const std::string& hash) = 0;
    
    /**
     * Get block header only (more efficient)
     */
    virtual StorageResult getBlockHeader(const std::string& hash, BlockHeader& header) const = 0;
    
    // === Transaction Storage ===
    
    /**
     * Store a transaction
     */
    virtual StorageResult putTransaction(const std::string& hash, const Transaction& tx) = 0;
    
    /**
     * Retrieve a transaction by hash
     */
    virtual StorageResult getTransaction(const std::string& hash, Transaction& tx) const = 0;
    
    /**
     * Check if transaction exists
     */
    virtual bool hasTransaction(const std::string& hash) const = 0;
    
    /**
     * Delete a transaction
     */
    virtual StorageResult deleteTransaction(const std::string& hash) = 0;
    
    // === UTXO Set Management ===
    
    /**
     * Store UTXO data
     */
    virtual StorageResult putUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) = 0;
    
    /**
     * Retrieve UTXO data
     */
    virtual StorageResult getUTXO(const std::string& outpoint, std::vector<uint8_t>& utxo_data) const = 0;
    
    /**
     * Check if UTXO exists
     */
    virtual bool hasUTXO(const std::string& outpoint) const = 0;
    
    /**
     * Delete UTXO
     */
    virtual StorageResult deleteUTXO(const std::string& outpoint) = 0;
    
    /**
     * Get all UTXOs for an address
     */
    virtual StorageResult getUTXOsForAddress(const std::string& address, 
                                           std::vector<std::pair<std::string, std::vector<uint8_t>>>& utxos) const = 0;
    
    // === Chain State ===
    
    /**
     * Store chain state data (best block, height, etc.)
     */
    virtual StorageResult putChainState(const std::string& key, const std::vector<uint8_t>& value) = 0;
    
    /**
     * Retrieve chain state data
     */
    virtual StorageResult getChainState(const std::string& key, std::vector<uint8_t>& value) const = 0;
    
    /**
     * Delete chain state data
     */
    virtual StorageResult deleteChainState(const std::string& key) = 0;
    
    // === Metadata Storage ===
    
    /**
     * Store arbitrary metadata
     */
    virtual StorageResult putMetadata(const std::string& key, const std::vector<uint8_t>& value) = 0;
    
    /**
     * Retrieve metadata
     */
    virtual StorageResult getMetadata(const std::string& key, std::vector<uint8_t>& value) const = 0;
    
    /**
     * Delete metadata
     */
    virtual StorageResult deleteMetadata(const std::string& key) = 0;
    
    // === Batch Operations ===
    
    /**
     * Create a write batch for atomic operations
     */
    virtual std::unique_ptr<WriteBatch> createWriteBatch() = 0;
    
    /**
     * Execute a write batch atomically
     */
    virtual StorageResult writeBatch(WriteBatch& batch) = 0;
    
    // === Iteration ===
    
    /**
     * Create iterator for range queries
     */
    virtual std::unique_ptr<StorageIterator> createIterator(const std::string& prefix = "") = 0;
    
    // === Maintenance ===
    
    /**
     * Compact storage (optimize space and performance)
     */
    virtual StorageResult compact() = 0;
    
    /**
     * Verify storage integrity
     */
    virtual StorageResult verify() = 0;
    
    /**
     * Get storage statistics
     */
    virtual StorageStats getStats() const = 0;
    
    /**
     * Get approximate size of key range (LevelDB specific)
     */
    virtual uint64_t getApproximateSize(const std::string& start_key, const std::string& end_key) const = 0;
    
    /**
     * Backup storage to directory
     */
    virtual StorageResult backup(const std::string& backup_dir) = 0;
    
    /**
     * Restore from backup
     */
    virtual StorageResult restore(const std::string& backup_dir) = 0;
    
    // === Configuration ===
    
    /**
     * Set storage option
     */
    virtual StorageResult setOption(const std::string& key, const std::string& value) = 0;
    
    /**
     * Get storage option
     */
    virtual std::string getOption(const std::string& key) const = 0;
    
    /**
     * Perform self-test to verify storage functionality
     */
    virtual bool selfTest() = 0;
    
    /**
     * Get backend name for logging
     */
    virtual std::string name() const = 0;
};

/**
 * Storage factory for creating different backend implementations
 */
class StorageFactory {
public:
    enum class BackendType {
        ROCKSDB,    // Production backend
        SQLITE,     // Development/testing backend
        MEMORY      // In-memory backend for tests
    };
    
    /**
     * Create storage backend instance
     */
    static std::unique_ptr<StorageInterface> create(BackendType type);
    static std::unique_ptr<StorageInterface> create(std::string_view name, bool allow_fallback = true);
    static std::unique_ptr<StorageInterface> create(const StorageConfig& config);
    static std::vector<std::string> getAvailableBackends();
    static std::string getBackendName(BackendType type);
    
    /**
     * Parse backend type from string
     */
    static std::optional<BackendType> parseBackendType(const std::string& name);
};

// Forward declaration - actual definition in storage_config.h
struct StorageConfig;

/**
 * Global storage instance
 */
extern std::unique_ptr<StorageInterface> g_storage;

/**
 * Initialize global storage
 */
StorageResult InitializeStorage(const StorageConfig& config);

/**
 * Shutdown global storage
 */
StorageResult ShutdownStorage();

} // namespace storage
} // namespace dinero
