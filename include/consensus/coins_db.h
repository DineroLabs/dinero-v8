#pragma once

#include "consensus/chain_state_view.h"
#include "consensus/utxo_entry.h"
#include "common/status.h"
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace dinero {

// Forward declaration
struct Transaction;

namespace consensus {

/**
 * Phase 23.0: CoinsDB - RocksDB-backed UTXO set
 *
 * Database Schema:
 * ================
 * UTXO entries:
 *   Key:   'C' + txid (32 bytes) + vout (4 bytes, little-endian)
 *   Value: Serialized UTXOEntry
 *
 * Undo data (for reorgs):
 *   Key:   'U' + block_hash (32 bytes)
 *   Value: Serialized UndoCoins
 *
 * Best UTXO set hash (for integrity):
 *   Key:   'B'
 *   Value: block_hash (32 bytes)
 *
 * This design follows Bitcoin Core's utxo database model.
 */
class CoinsDB {
public:
    CoinsDB();
    ~CoinsDB();

    // Initialize the database
    Status open(const std::string& db_path);
    void close();

    // ========================================================================
    // UTXO Lookup Operations
    // ========================================================================

    /**
     * Get a UTXO entry by outpoint
     * Returns Status::Ok if found, Status::NotFound if not exists
     */
    StatusOr<UTXOEntry> getCoin(const OutPoint& outpoint) const;

    /**
     * Check if a UTXO exists
     */
    bool hasCoin(const OutPoint& outpoint) const;

    // ========================================================================
    // UTXO Modification Operations
    // ========================================================================

    /**
     * Add a new UTXO to the set
     * Called when a transaction output is created
     */
    Status addCoin(const OutPoint& outpoint, const UTXOEntry& coin);

    /**
     * Spend (remove) a UTXO from the set
     * Called when a transaction input is consumed
     * Returns the spent coin for undo data
     */
    StatusOr<UTXOEntry> spendCoin(const OutPoint& outpoint);

    /**
     * Batch write multiple UTXO changes atomically
     * Used during block application/undo
     */
    Status writeBatch(
        const std::vector<std::pair<OutPoint, UTXOEntry>>& coins_to_add,
        const std::vector<OutPoint>& coins_to_spend);

    // ========================================================================
    // Undo Data Operations (for reorgs)
    // ========================================================================

    /**
     * Store undo data for a block
     * Allows reverting block application during reorg
     */
    Status writeUndoCoins(const std::string& block_hash, const UndoCoins& undo);

    /**
     * Retrieve undo data for a block
     */
    StatusOr<UndoCoins> getUndoCoins(const std::string& block_hash) const;

    /**
     * Delete undo data for a block
     * Called after the block is deeply buried (100+ confirmations)
     */
    Status deleteUndoCoins(const std::string& block_hash);

    // ========================================================================
    // Best Block Operations
    // ========================================================================

    /**
     * Store the block hash representing the current UTXO set state
     */
    Status writeBestBlock(const std::string& block_hash);

    /**
     * Get the block hash representing the current UTXO set state
     */
    StatusOr<std::string> getBestBlock() const;

    // ========================================================================
    // Statistics and Maintenance
    // ========================================================================

    /**
     * Get approximate UTXO set size
     */
    uint64_t getUtxoCount() const;

    /**
     * Iterate through all UTXOs (for gettxoutsetinfo, snapshot generation, etc.)
     *
     * Callback signature: void callback(const OutPoint& outpoint, const UTXOEntry& coin)
     * Returns true if iteration completed successfully
     */
    template<typename Callback>
    bool iterateAllCoins(Callback&& callback) const;

    /**
     * Get database size in bytes
     */
    uint64_t getDatabaseSize() const;

    /**
     * Flush pending writes to disk
     */
    Status flush();

private:
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::Options options_;

    // Key encoding functions
    static std::string encodeCoinKey(const OutPoint& outpoint);
    static std::string encodeUndoKey(const std::string& block_hash);
    static OutPoint decodeCoinKey(const std::string& key);

    // Serialization functions
    static std::string serializeUTXOEntry(const UTXOEntry& coin);
    static StatusOr<UTXOEntry> deserializeUTXOEntry(const std::string& data);
    static std::string serializeUndoCoins(const UndoCoins& undo);
    static StatusOr<UndoCoins> deserializeUndoCoins(const std::string& data);

    // Internal helpers
    Status getImpl(const std::string& key, std::string& value) const;
    Status putImpl(const std::string& key, const std::string& value);
    Status deleteImpl(const std::string& key);
};

/**
 * CoinsViewCache - In-memory cache for UTXO lookups
 *
 * Provides fast access to UTXOs during validation without hitting RocksDB
 * for every lookup. Changes are batched and written to CoinsDB atomically.
 *
 * This is critical for performance:
 * - Block validation needs thousands of UTXO lookups
 * - RocksDB reads are slower than RAM
 * - Write batching reduces I/O
 */
/**
 * CoinsViewCache - L1 UTXO cache for block validation
 *
 * CONST-CORRECTNESS NOTE:
 * CoinsViewCache is logically const for read operations (getCoin, hasCoin, getHeight).
 * Internal mutation (cached_coins_) is allowed to preserve existing caching semantics.
 *
 * Thread-safety: Read operations modify internal cache state but are safe
 * when used correctly (single-threaded block validation, mempool validation).
 *
 * Do NOT refactor to remove mutable - this is intentional for performance.
 */
class CoinsViewCache : public ChainStateView {
public:
    explicit CoinsViewCache(CoinsDB* base_db);
    ~CoinsViewCache();

    // ========================================================================
    // ChainStateView interface (read-only, const-correct)
    // ========================================================================

    // UTXO lookup (checks cache first, then falls back to DB)
    StatusOr<UTXOEntry> getCoin(const OutPoint& outpoint) const override;
    bool hasCoin(const OutPoint& outpoint) const override;
    uint32_t getHeight() const override;

    // ========================================================================
    // Mutable operations (block connection/disconnection only)
    // ========================================================================

    // UTXO modifications (cached, not written to DB yet)
    void addCoin(const OutPoint& outpoint, const UTXOEntry& coin);
    StatusOr<UTXOEntry> spendCoin(const OutPoint& outpoint);

    // Flush cache to underlying database
    Status flush();

    // Clear the cache without writing (rollback)
    void clear();

    // Update current height (called when blocks are connected/disconnected)
    void setHeight(uint32_t height);

    // Get cache statistics
    size_t getCacheSize() const;
    size_t getAddedCount() const;
    size_t getSpentCount() const;

    // ========================================================================
    // Phase 23.1.F: Apply Transaction to UTXO Set
    // ========================================================================

    /**
     * Apply transaction to UTXO set (spend inputs, add outputs)
     *
     * This is the core of block validation:
     * 1. Spend all inputs (remove from UTXO set)
     * 2. Add all outputs (add to UTXO set)
     * 3. Generate undo data (for reorgs)
     *
     * This method DOES NOT validate the transaction - call validateTransaction() first!
     * This method DOES NOT write to database - call flush() after block validation!
     *
     * @param tx          Transaction to apply
     * @param txid        Transaction ID (hash)
     * @param height      Block height where transaction is included
     * @param is_coinbase True if this is a coinbase transaction
     * @param undo        Output: Undo data (spent coins for reorg)
     * @return            True if successful, false if input not found
     */
    bool applyTransaction(
        const struct Transaction& tx,
        const uint256& txid,
        uint32_t height,
        bool is_coinbase,
        UndoCoins& undo
    );

    /**
     * Undo transaction (restore spent inputs, remove created outputs)
     *
     * Used during block disconnection (reorg):
     * 1. Remove all outputs created by this transaction
     * 2. Restore all inputs spent by this transaction (from undo data)
     *
     * @param tx          Transaction to undo
     * @param txid        Transaction ID (hash)
     * @param undo        Undo data (spent coins to restore)
     * @return            True if successful
     */
    bool undoTransaction(
        const struct Transaction& tx,
        const uint256& txid,
        const UndoCoins& undo
    );

private:
    CoinsDB* base_db_;

    // Cache state:
    // - added_coins_: UTXOs created but not yet in DB
    // - spent_coins_: OutPoints spent but not yet removed from DB
    // - cached_coins_: UTXOs loaded from DB (read cache)
    std::unordered_map<OutPoint, UTXOEntry> added_coins_;
    std::unordered_set<OutPoint> spent_coins_;
    mutable std::unordered_map<OutPoint, UTXOEntry> cached_coins_;

    // Blockchain height (updated via setHeight())
    uint32_t current_height_;

    // Track dirty state
    bool has_changes_;
};

// ============================================================================
// Template Implementation (must be in header for templates)
// ============================================================================

template<typename Callback>
bool CoinsDB::iterateAllCoins(Callback&& callback) const {
    if (!db_) {
        return false;
    }

    rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());

    // Seek to first UTXO (key starts with 'C')
    for (it->Seek("C"); it->Valid() && it->key().starts_with("C"); it->Next()) {
        // Decode key (OutPoint)
        OutPoint outpoint = decodeCoinKey(it->key().ToString());
        if (outpoint.txid.IsNull()) {  // Phase M.0: uint256 uses IsNull() not empty()
            continue;  // Skip invalid keys
        }

        // Decode value (UTXOEntry)
        auto coin_result = deserializeUTXOEntry(it->value().ToString());
        if (!coin_result.ok()) {
            continue;  // Skip invalid entries
        }

        // Call user callback
        callback(outpoint, coin_result.value());
    }

    bool success = it->status().ok();
    delete it;
    return success;
}

} // namespace consensus
} // namespace dinero
