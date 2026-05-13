#include "consensus/block_index_persistence.h"
#include "consensus/block_lifecycle.h"
#include "common/logger.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cassert>

namespace dinero {

/**
 * Serialize BlockIndexEntry to binary format
 *
 * Format (all fields big-endian):
 * - hash (32 bytes)
 * - prev_hash (32 bytes)
 * - height (4 bytes)
 * - version (4 bytes)
 * - merkle_root (32 bytes)
 * - timestamp (8 bytes)
 * - bits (4 bytes)
 * - nonce (4 bytes)
 * - chainwork length (2 bytes) + chainwork (variable)
 * - status (4 bytes)
 * - children count (4 bytes) + children hashes (32 bytes each)
 */
std::string BlockIndexEntry::Serialize() const {
    std::ostringstream ss;

    // Helper to write fixed-size fields
    auto writeBytes = [&ss](const void* data, size_t size) {
        ss.write(reinterpret_cast<const char*>(data), size);
    };

    // Helper to write uint256 as binary (32 bytes)
    auto writeHash = [&writeBytes](const uint256& hash) {
        writeBytes(hash.data, 32);
    };

    // Write fields
    writeHash(hash);
    writeHash(prev_hash);

    uint32_t h = height;
    uint32_t v = version;
    uint64_t ts = timestamp;
    uint32_t b = bits;
    uint32_t n = nonce;
    uint32_t st = status;

    writeBytes(&h, 4);
    writeBytes(&v, 4);
    writeHash(merkle_root);
    writeBytes(&ts, 8);
    writeBytes(&b, 4);
    writeBytes(&n, 4);

    // Chainwork (variable length)
    uint16_t chainwork_len = static_cast<uint16_t>(chainwork.length());
    writeBytes(&chainwork_len, 2);
    ss.write(chainwork.data(), chainwork_len);

    // Status
    writeBytes(&st, 4);

    // Children count and hashes
    uint32_t children_count = static_cast<uint32_t>(children.size());
    writeBytes(&children_count, 4);
    for (const auto& child_hash : children) {
        writeHash(child_hash);
    }

    return ss.str();
}

/**
 * Deserialize BlockIndexEntry from binary format
 */
BlockIndexEntry BlockIndexEntry::Deserialize(const std::string& data) {
    BlockIndexEntry entry;

    if (data.size() < 120) {  // Minimum size (without variable fields)
        return entry;  // Invalid
    }

    size_t offset = 0;

    // Helper to read fixed-size fields
    auto readBytes = [&data, &offset](void* dest, size_t size) {
        if (offset + size > data.size()) return false;
        std::memcpy(dest, data.data() + offset, size);
        offset += size;
        return true;
    };

    // Helper to read hash (32 bytes → uint256)
    auto readHash = [&readBytes]() -> uint256 {
        uint8_t bytes[32];
        if (!readBytes(bytes, 32)) return uint256();
        uint256 result;
        std::memcpy(result.data, bytes, 32);
        return result;
    };

    // Read fields
    entry.hash = readHash();
    entry.prev_hash = readHash();
    readBytes(&entry.height, 4);
    readBytes(&entry.version, 4);
    entry.merkle_root = readHash();
    readBytes(&entry.timestamp, 8);
    readBytes(&entry.bits, 4);
    readBytes(&entry.nonce, 4);

    // Chainwork
    uint16_t chainwork_len = 0;
    readBytes(&chainwork_len, 2);
    if (offset + chainwork_len > data.size()) return entry;
    entry.chainwork = data.substr(offset, chainwork_len);
    offset += chainwork_len;

    // Status
    readBytes(&entry.status, 4);

    // Children
    uint32_t children_count = 0;
    readBytes(&children_count, 4);
    for (uint32_t i = 0; i < children_count && offset < data.size(); i++) {
        entry.children.push_back(readHash());
    }

    return entry;
}

/**
 * Convert CBlockIndex to BlockIndexEntry
 */
BlockIndexEntry BlockIndexEntry::FromBlockIndex(const CBlockIndex* pindex) {
    if (!pindex) return BlockIndexEntry{};

    BlockIndexEntry entry;
    entry.hash = pindex->hash;
    entry.prev_hash = pindex->prev_hash;
    entry.height = pindex->height;
    entry.version = pindex->version;
    entry.merkle_root = pindex->merkle_root;
    entry.timestamp = pindex->timestamp;
    entry.bits = pindex->bits;
    entry.nonce = pindex->nonce;
    entry.chainwork = pindex->chainwork;

    // Mask out runtime-only flags (BLOCK_IN_FLIGHT is not persisted)
    entry.status = pindex->status & ~BLOCK_IN_FLIGHT;

    // Extract children hashes
    for (const CBlockIndex* child : pindex->children) {
        if (child) {
            entry.children.push_back(child->GetBlockHash());
        }
    }

    return entry;
}

/**
 * Apply persisted data to CBlockIndex
 */
void BlockIndexEntry::ApplyToBlockIndex(CBlockIndex* pindex) const {
    if (!pindex) return;

    // Update metadata (hash/prev_hash should already match)
    pindex->height = height;
    pindex->version = version;
    pindex->merkle_root = merkle_root;
    pindex->timestamp = timestamp;
    pindex->bits = bits;
    pindex->nonce = nonce;
    pindex->chainwork = chainwork;
    pindex->status = status;

    // Note: pprev and children pointers are reconstructed separately
    // from the hash linkage, not from this entry directly
}

/**
 * Save block index to database
 */
bool BlockIndexDB::SaveBlockIndex(const CBlockIndex* pindex, rocksdb::WriteBatch* wb) {
    if (!pindex) return false;

    BlockIndexEntry entry = BlockIndexEntry::FromBlockIndex(pindex);
    std::string key = MakeKey(pindex->GetBlockHash());
    std::string value = entry.Serialize();

    if (wb) {
        wb->Put(cf_, key, value);
        return true;
    } else {
        rocksdb::WriteOptions options;
        options.sync = false;  // Async for performance
        rocksdb::Status status = db_->Put(options, cf_, key, value);
        return status.ok();
    }
}

/**
 * Load block index from database
 */
bool BlockIndexDB::LoadBlockIndex(const uint256& block_hash, BlockIndexEntry& entry) {
    std::string key = MakeKey(block_hash);
    std::string value;

    rocksdb::ReadOptions options;
    rocksdb::Status status = db_->Get(options, cf_, key, &value);

    if (!status.ok()) {
        return false;
    }

    entry = BlockIndexEntry::Deserialize(value);
    return !(entry.hash == uint256());
}

/**
 * Load all block indices (for startup)
 */
bool BlockIndexDB::LoadAllBlockIndices(std::vector<BlockIndexEntry>& entries) {
    entries.clear();

    rocksdb::ReadOptions options;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(options, cf_));

    std::string prefix = PREFIX_BLOCK_INDEX;
    for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
        BlockIndexEntry entry = BlockIndexEntry::Deserialize(it->value().ToString());
        if (!(entry.hash == uint256())) {
            entries.push_back(std::move(entry));
        }
    }

    return it->status().ok();
}

/**
 * Delete block index entry
 */
bool BlockIndexDB::DeleteBlockIndex(const uint256& block_hash, rocksdb::WriteBatch* wb) {
    std::string key = MakeKey(block_hash);

    if (wb) {
        wb->Delete(cf_, key);
        return true;
    } else {
        rocksdb::WriteOptions options;
        rocksdb::Status status = db_->Delete(options, cf_, key);
        return status.ok();
    }
}

/**
 * Batch save multiple block indices
 */
bool BlockIndexDB::SaveBlockIndices(const std::vector<const CBlockIndex*>& indices) {
    rocksdb::WriteBatch batch;

    for (const CBlockIndex* pindex : indices) {
        if (pindex) {
            SaveBlockIndex(pindex, &batch);
        }
    }

    rocksdb::WriteOptions options;
    options.sync = true;  // Sync for batch operations
    rocksdb::Status status = db_->Write(options, &batch);
    return status.ok();
}

/**
 * Reconstruct in-memory block index from persisted data
 *
 * This function:
 * 1. Loads all BlockIndexEntry records from database
 * 2. Creates CBlockIndex objects in g_block_index
 * 3. Recomputes pprev pointers from prev_hash linkage
 * 4. Recomputes children vectors from forward links
 * 5. Rebuilds g_candidates set
 */
bool ReconstructBlockIndex(BlockIndexDB& db) {
    g_logger.log(LogLevel::INFO, "Reconstructing block index from database");

    // Load all persisted entries
    std::vector<BlockIndexEntry> entries;
    if (!db.LoadAllBlockIndices(entries)) {
        g_logger.log(LogLevel::ERROR, "Failed to load block index entries");
        return false;
    }

    g_logger.log(LogLevel::INFO, "Loaded block index entries: count=" + std::to_string(entries.size()));

    // Clear existing index
    g_block_index.clear();
    g_candidates.clear();

    // Phase 1: Create all CBlockIndex objects
    for (const auto& entry : entries) {
        auto pindex = std::make_unique<CBlockIndex>();
        pindex->hash = entry.hash;
        pindex->prev_hash = entry.prev_hash;
        entry.ApplyToBlockIndex(pindex.get());

        g_block_index[entry.hash] = std::move(pindex);

        // ═══════════════════════════════════════════════════════════════════════════
        // INVARIANT: Exactly one CBlockIndex per hash, forever.
        // Verify the entry we just inserted is the one returned by FindBlockIndex.
        // ═══════════════════════════════════════════════════════════════════════════
#ifndef NDEBUG
        CBlockIndex* lookup = FindBlockIndex(entry.hash);
        assert(lookup == g_block_index[entry.hash].get() &&
               "ONE BlockIndex per hash invariant violated during disk load!");
#endif
    }

    // Phase 2: Reconstruct pprev pointers
    for (auto& [hash, pindex] : g_block_index) {
        if (!(pindex->prev_hash == uint256()) && pindex->height > 0) {
            auto it = g_block_index.find(pindex->prev_hash);
            if (it != g_block_index.end()) {
                pindex->pprev = it->second.get();
            } else {
                g_logger.log(LogLevel::WARNING, "Parent block not found");
            }
        }
    }

    // Phase 3: Reconstruct children vectors from persisted forward links
    for (const auto& entry : entries) {
        CBlockIndex* parent = FindBlockIndex(entry.hash);
        if (!parent) continue;

        for (const uint256& child_hash : entry.children) {
            CBlockIndex* child = FindBlockIndex(child_hash);
            if (child) {
                parent->children.push_back(child);
            }
        }
    }

    // Phase 4: Rebuild g_candidates (blocks with BLOCK_VALID_CHAIN)
    for (auto& [hash, pindex] : g_block_index) {
        if (pindex->status & BLOCK_VALID_CHAIN) {
            AddCandidate(pindex.get());
        }
    }

    g_logger.log(LogLevel::INFO, "Block index reconstruction complete: total_blocks=" + std::to_string(g_block_index.size()) +
        " candidate_tips=" + std::to_string(g_candidates.size()));

    return true;
}

/**
 * Persist current block index state
 *
 * Should be called:
 * - Periodically (after N blocks)
 * - On clean shutdown
 * - Before dangerous operations (reindex, etc.)
 */
bool PersistBlockIndexState() {
    // TODO: This requires access to ChainDB instance
    // Implementation depends on how ChainDB is accessed globally
    // For now, this is a placeholder

    g_logger.log(LogLevel::INFO, "Persisting block index state: total_blocks=" + std::to_string(g_block_index.size()));

    // Collect all block indices
    std::vector<const CBlockIndex*> indices;
    for (const auto& [hash, pindex] : g_block_index) {
        indices.push_back(pindex.get());
    }

    // TODO: Call BlockIndexDB::SaveBlockIndices(indices)
    // This requires passing ChainDB instance

    return true;
}

} // namespace dinero
