# Phase 19: Flat File Storage & BlockIndex - Implementation Guide

## Architecture Overview

DineroCoin has migrated from storing full blocks in RocksDB to Bitcoin-style flat file storage with an in-memory BlockIndex.

### Storage Layout

```
datadir/
├── blocks/
│   ├── blk00000.dat          # Raw block bodies (append-only)
│   ├── blk00001.dat
│   └── ...
└── chaindb/                   # RocksDB
    ├── block_index/           # hash → BlockIndex metadata
    ├── height_index/          # height → hash
    ├── tip/                   # Current chain tip
    └── utxo/                  # UTXO set
```

### Three-Layer Architecture

1. **Disk: Flat Files** (`BlockStorage`)
   - Raw block data in `blk*.dat` files
   - 128MB per file (Bitcoin standard)
   - Format: `[magic:4][size:4][block_data:N]`
   - Network magic bytes (mainnet/testnet/regtest)

2. **Disk: RocksDB Index** (`ChainDB`)
   - BlockIndex metadata (NOT full blocks)
   - FilePosition references to flat files
   - Chain state (tip, height, work)

3. **Memory: BlockIndex DAG**
   - In-memory graph with `pprev`/`pnext` pointers
   - Skip list for O(log n) ancestor lookups
   - Fast chain traversal without disk I/O

---

## Critical Implementation Details

### 1. Block Acceptance - Crash Consistency

**SAFE ORDER OF OPERATIONS:**

```cpp
Status AcceptBlock(const Block& block, const uint256& hash) {
    // ===================================================================
    // STEP 1: Validate block FULLY before any writes
    // ===================================================================
    Status validation = ValidateBlock(block);
    if (!validation.ok()) {
        return validation;
    }

    // ===================================================================
    // STEP 2: Write block to flat file storage
    // ===================================================================
    auto pos_result = block_storage_.writeBlock(hash, block);
    if (!pos_result.ok()) {
        // SAFE: Nothing committed to index yet
        LOG_ERROR("Failed to write block to flat file: " + pos_result.status().message());
        return pos_result.status();
    }
    FilePosition file_pos = pos_result.value();

    // ===================================================================
    // STEP 3: Create BlockIndex with FilePosition
    // ===================================================================
    BlockIndex index(block.header);
    index.hash = hash;
    index.height = current_height + 1;
    index.file_pos = file_pos;
    index.tx_count = block.vtx.size();
    index.chain_work = parent_work + GetBlockWork(block);
    index.status = BlockIndex::VALID_TRANSACTIONS | BlockIndex::BLOCK_HAVE_DATA;

    // ===================================================================
    // STEP 4: Atomic RocksDB write batch
    // ===================================================================
    rocksdb::WriteBatch batch;

    // Write BlockIndex
    std::string index_data = index.serialize();
    batch.Put(cf_block_index, hash.GetHex(), index_data);

    // Update height index
    batch.Put(cf_height_index, std::to_string(index.height), hash.GetHex());

    // Update tip if this extends best chain
    if (index.chain_work > current_tip_work) {
        TipInfo new_tip(hash, index.height, index.chain_work);
        batch.Put(cf_meta, "tip", new_tip.serialize());
    }

    // ===================================================================
    // STEP 5: COMMIT - Point of no return
    // ===================================================================
    Status commit = chaindb_->writeBatch(std::move(batch), /*sync=*/true);
    if (!commit.ok()) {
        // DANGER: Block is on disk but unindexed
        // Solution: Log for manual recovery or reindex
        LOG_CRITICAL("Block written to disk but index commit failed!");
        LOG_CRITICAL("  Block hash: " + hash.GetHex());
        LOG_CRITICAL("  File position: file=" + std::to_string(file_pos.file_number) +
                     " offset=" + std::to_string(file_pos.offset));
        return commit;
    }

    // ===================================================================
    // STEP 6: Update in-memory BlockIndex DAG (after commit)
    // ===================================================================
    block_index_map_[hash] = std::make_unique<BlockIndex>(index);
    block_index_map_[hash]->pprev = parent_index;
    if (is_best_chain) {
        parent_index->pnext = block_index_map_[hash].get();
    }
    block_index_map_[hash]->buildSkip();

    return Status::OK();
}
```

### 2. Crash Recovery Scenarios

**Scenario A: Crash after flat file write, before index commit**
```
State:
  - blk00000.dat contains block data
  - No index entry for block hash

Recovery:
  - Block is "orphaned" on disk
  - Safe to ignore (only trust indexed blocks)
  - Optional: Reindex can recover orphaned blocks
```

**Scenario B: Crash after index commit**
```
State:
  - blk00000.dat contains block data
  - Index entry exists with FilePosition

Recovery:
  - Fully consistent
  - Normal startup resumes
```

**Scenario C: Index commit fails (disk full, corruption)**
```
State:
  - Block data written to blk*.dat
  - RocksDB write failed

Action:
  - Log CRITICAL error with block hash + file position
  - Operator can:
    a) Run reindex to rebuild index from flat files
    b) Manually recover using logged FilePosition
  - DO NOT silently continue
```

### 3. Block Retrieval - Error Handling

**SAFE getBlock() IMPLEMENTATION:**

```cpp
StatusOr<Block> ChainDB::getBlock(const uint256& hash) const {
    // ===================================================================
    // STEP 1: Read FilePosition from index
    // ===================================================================
    std::string index_data;
    auto status = db_->Get(rocksdb::ReadOptions(), cf_block_index_,
                           hash.GetHex(), &index_data);

    if (status.IsNotFound()) {
        return Status::NotFound("Block not in index: " + hash.GetHex());
    }
    if (!status.ok()) {
        return Status::IOError("Index read failed: " + status.ToString());
    }

    // ===================================================================
    // STEP 2: Deserialize BlockIndex to get FilePosition
    // ===================================================================
    BlockIndex index;
    try {
        index = BlockIndex::deserialize(index_data);
    } catch (const std::exception& e) {
        return Status::Serialization("Failed to deserialize BlockIndex: " +
                                     std::string(e.what()));
    }

    // ===================================================================
    // STEP 3: Validate FilePosition sanity
    // ===================================================================
    if (index.file_pos.isNull()) {
        return Status::Corruption("BlockIndex has null FilePosition");
    }

    // ===================================================================
    // STEP 4: Read from flat file with comprehensive error checking
    // ===================================================================
    auto block_result = block_storage_.readBlock(index.file_pos);

    if (!block_result.ok()) {
        Status err = block_result.status();

        // Log specific error for debugging
        if (err == Status::NotFound) {
            LOG_ERROR("Block file not found: blk" +
                     std::to_string(index.file_pos.file_number) + ".dat");
        } else if (err == Status::IOError) {
            LOG_ERROR("I/O error reading block at offset " +
                     std::to_string(index.file_pos.offset));
        } else if (err == Status::Corruption) {
            LOG_ERROR("Magic bytes or size mismatch for block " + hash.GetHex());
        }

        return err;
    }

    // ===================================================================
    // STEP 5: Verify block hash matches
    // ===================================================================
    Block block = block_result.value();
    uint256 actual_hash(block.GetHash());

    if (actual_hash != hash) {
        LOG_CRITICAL("Block hash mismatch!");
        LOG_CRITICAL("  Expected: " + hash.GetHex());
        LOG_CRITICAL("  Actual:   " + actual_hash.GetHex());
        LOG_CRITICAL("  File:     blk" + std::to_string(index.file_pos.file_number) +
                     ".dat @ " + std::to_string(index.file_pos.offset));
        return Status::Corruption("Block hash verification failed");
    }

    return block;
}
```

### 4. Startup - Loading BlockIndex into Memory

**REQUIRED ON NODE STARTUP:**

```cpp
Status LoadBlockIndex() {
    LOG_INFO("Loading block index from disk...");

    std::unordered_map<uint256, std::unique_ptr<BlockIndex>> temp_map;

    // ===================================================================
    // STEP 1: Load all BlockIndex entries from RocksDB
    // ===================================================================
    rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions(), cf_block_index_);

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        uint256 hash(it->key().ToString());

        try {
            BlockIndex index = BlockIndex::deserialize(it->value().ToString());
            temp_map[hash] = std::make_unique<BlockIndex>(index);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to deserialize BlockIndex for " + hash.GetHex());
            continue;
        }
    }

    delete it;

    LOG_INFO("Loaded " + std::to_string(temp_map.size()) + " block index entries");

    // ===================================================================
    // STEP 2: Build pprev/pnext linkage
    // ===================================================================
    for (auto& [hash, index] : temp_map) {
        if (index->hash_prev.IsNull()) {
            // Genesis block
            index->pprev = nullptr;
        } else {
            auto parent_it = temp_map.find(index->hash_prev);
            if (parent_it != temp_map.end()) {
                index->pprev = parent_it->second.get();
            } else {
                LOG_WARNING("Parent block not found for " + hash.GetHex());
            }
        }
    }

    // ===================================================================
    // STEP 3: Build skip list for efficient ancestor lookups
    // ===================================================================
    for (auto& [hash, index] : temp_map) {
        index->buildSkip();
    }

    // ===================================================================
    // STEP 4: Find best chain tip
    // ===================================================================
    BlockIndex* best_tip = nullptr;
    arith_uint256 best_work = 0;

    for (auto& [hash, index] : temp_map) {
        if (index->chain_work > best_work) {
            best_work = index->chain_work;
            best_tip = index.get();
        }
    }

    if (!best_tip) {
        return Status::Corruption("No valid chain tip found");
    }

    // ===================================================================
    // STEP 5: Set active chain
    // ===================================================================
    BlockIndex* pindex = best_tip;
    while (pindex) {
        pindex->status |= BlockIndex::VALID_CHAIN;

        if (pindex->pprev) {
            pindex->pprev->pnext = pindex;
        }

        pindex = pindex->pprev;
    }

    // ===================================================================
    // STEP 6: Commit to global state
    // ===================================================================
    block_index_map_ = std::move(temp_map);
    chain_tip_ = best_tip;

    LOG_INFO("Best chain: height=" + std::to_string(best_tip->height) +
             " hash=" + best_tip->hash.GetHex());

    return Status::OK();
}
```

---

## What This Architecture Enables

### ✅ **Efficient Reorgs**
- Walk chain backward via `pprev` pointers (no disk I/O)
- Find common ancestor in O(log n) using skip list
- Atomic batch updates to switch chain tips

### ✅ **Orphan Handling**
- Build multi-branch DAG in memory
- Track competing chains by chain work
- Connect orphans when parent arrives

### ✅ **Parallel Block Validation**
- Headers-only validation (no full block reads)
- Build header chain independently
- Download blocks in parallel after headers validated

### ✅ **P2P Syncing**
- Fast block locator construction (skip list traversal)
- Common ancestor finding for IBD
- Getheaders/getdata efficiency

### ✅ **Compact Blocks (Phase 20)**
- Quick lookups: header, tx_count, height
- Reconstruct blocks from mempool
- Skip list for dependency checking

### ✅ **Pruning**
- File references enable old block deletion
- Keep BlockIndex, delete blk*.dat
- Retain last N blocks for reorg safety

### ✅ **Headers-First IBD**
- Download all headers first (lightweight)
- Validate PoW before downloading blocks
- Parallel block download

### ✅ **Lightning Network Integration**
- Height tracking for HTLC timeouts
- Block confirmation windows
- Fast UTXO lookups

---

## Testing Checklist

### Unit Tests
- [ ] BlockStorage write/read roundtrip
- [ ] FilePosition serialization
- [ ] BlockIndex serialization
- [ ] Skip list correctness
- [ ] Magic bytes validation

### Integration Tests
- [ ] Fresh node sync from genesis
- [ ] Node restart with existing data
- [ ] Block acceptance with file rotation
- [ ] getblock RPC with flat files
- [ ] Reindex from flat files

### Crash Recovery Tests
- [ ] Crash after flat file write, before commit
- [ ] Crash during RocksDB batch write
- [ ] Corrupted flat file handling
- [ ] Missing flat file handling
- [ ] Index/disk mismatch detection

### Performance Tests
- [ ] Measure block write throughput
- [ ] Measure block read latency
- [ ] Memory usage with 100K blocks
- [ ] Reorg performance (1-block, 10-block, 100-block)

---

## Migration Path (Existing Nodes)

For nodes that already have blocks in RocksDB:

```cpp
Status MigrateToFlatFiles() {
    LOG_INFO("Starting migration to flat file storage...");

    // 1. Iterate all blocks in old RocksDB format
    // 2. Write each block to flat file storage
    // 3. Create BlockIndex with FilePosition
    // 4. Write new index format
    // 5. Delete old block data from RocksDB
    // 6. Set migration flag in metadata

    // Atomic: All or nothing
    // Resumable: Track last migrated height
}
```

---

## Performance Characteristics

### Block Write
- **Before**: 1 RocksDB write (full block)
- **After**: 1 flat file append + 1 RocksDB write (16-byte FilePosition)
- **Speedup**: ~10-50x (large blocks)

### Block Read
- **Before**: 1 RocksDB read (full block)
- **After**: 1 RocksDB read (metadata) + 1 file seek/read
- **Impact**: Slightly slower (but enables caching)

### Chain Traversal
- **Before**: RocksDB read per block
- **After**: In-memory pointer traversal
- **Speedup**: ~1000x

### Reorg (100 blocks)
- **Before**: 100 RocksDB reads + 100 writes
- **After**: In-memory pprev walk + 1 batch write
- **Speedup**: ~100x

---

## Security Considerations

### 1. File System Permissions
- Flat files must be readable only by daemon user
- Prevent external modification of blk*.dat

### 2. Magic Byte Validation
- ALWAYS verify magic bytes on read
- Detect file corruption or tampering

### 3. Hash Verification
- Verify block hash after deserialization
- Prevent serving wrong blocks

### 4. Index Corruption Detection
- Check FilePosition bounds
- Validate file_number exists
- Verify offset + size within file bounds

---

## Monitoring & Metrics

Recommended metrics to track:

```cpp
struct BlockStorageMetrics {
    uint64_t blocks_written;
    uint64_t blocks_read;
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint64_t file_rotations;
    uint64_t read_errors;
    uint64_t write_errors;
    uint64_t corruption_detected;

    // Performance
    double avg_write_latency_ms;
    double avg_read_latency_ms;
    double p99_read_latency_ms;
};
```

---

## References

- Bitcoin Core `CBlockIndex`: https://github.com/bitcoin/bitcoin/blob/master/src/chain.h
- Bitcoin Core flat files: https://github.com/bitcoin/bitcoin/blob/master/src/flatfile.h
- BIP 152 (Compact Blocks): https://github.com/bitcoin/bips/blob/master/bip-0152.mediawiki

---

**Document Version**: 1.0
**Last Updated**: 2025-12-06
**Status**: Implementation in progress
