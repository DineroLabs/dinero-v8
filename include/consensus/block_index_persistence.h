#pragma once

#include <string>
#include <vector>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include "consensus/block_index.h"

namespace dinero {

/**
 * Block Index Persistence Layer
 *
 * Persists CBlockIndex metadata to RocksDB for efficient restart.
 * This completes F.4 by ensuring all block lifecycle state survives restarts.
 *
 * Persisted Data:
 * - Block status flags (validation state, data availability)
 * - Forward links (children hashes)
 * - Chainwork
 * - Height
 *
 * NOT persisted (computed at runtime):
 * - pprev pointer (recomputed from prev_hash on load)
 * - children pointer vector (recomputed from forward link hashes)
 * - In-flight flags (runtime-only state)
 */

/**
 * Serialized block index entry
 */
struct BlockIndexEntry {
    uint256 hash;
    uint256 prev_hash;
    uint32_t height;
    uint32_t version;
    uint256 merkle_root;
    uint64_t timestamp;
    uint32_t bits;
    uint32_t nonce;
    std::string chainwork;
    uint32_t status;                      // Validation and data flags
    std::vector<uint256> children;        // Forward link hashes

    // Serialize to binary format
    std::string Serialize() const;

    // Deserialize from binary format
    static BlockIndexEntry Deserialize(const std::string& data);

    // Convert from CBlockIndex
    static BlockIndexEntry FromBlockIndex(const CBlockIndex* pindex);

    // Apply to CBlockIndex (updates runtime state)
    void ApplyToBlockIndex(CBlockIndex* pindex) const;
};

/**
 * Block index persistence manager
 */
class BlockIndexDB {
public:
    BlockIndexDB(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf)
        : db_(db), cf_(cf) {}

    // Save block index entry
    bool SaveBlockIndex(const CBlockIndex* pindex, rocksdb::WriteBatch* wb = nullptr);

    // Load block index entry
    bool LoadBlockIndex(const uint256& block_hash, BlockIndexEntry& entry);

    // Load all block indices (for startup reconstruction)
    bool LoadAllBlockIndices(std::vector<BlockIndexEntry>& entries);

    // Delete block index entry
    bool DeleteBlockIndex(const uint256& block_hash, rocksdb::WriteBatch* wb = nullptr);

    // Batch persistence
    bool SaveBlockIndices(const std::vector<const CBlockIndex*>& indices);

private:
    rocksdb::DB* db_;
    rocksdb::ColumnFamilyHandle* cf_;

    static constexpr const char* PREFIX_BLOCK_INDEX = "bi:";  // Block index prefix

    std::string MakeKey(const uint256& block_hash) const {
        return std::string(PREFIX_BLOCK_INDEX) + block_hash.GetHex();
    }
};

/**
 * Block index reconstruction on startup
 *
 * Rebuilds in-memory CBlockIndex structures from persisted metadata.
 * Recomputes pprev/children pointers from hash linkage.
 */
bool ReconstructBlockIndex(BlockIndexDB& db);

/**
 * Persist current block index state
 *
 * Should be called periodically (e.g., after N blocks) and on shutdown.
 */
bool PersistBlockIndexState();

} // namespace dinero
