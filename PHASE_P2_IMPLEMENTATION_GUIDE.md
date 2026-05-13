# Phase P.2 Implementation Guide - Execution Checklist

**Date:** December 19, 2025
**Status:** 60% Complete - Ready for Mechanical Completion
**Session Goal:** Follow this checklist to completion without architectural decisions

---

## 1. Invariant Recap (Non-Negotiable Truth)

### What Pruning MAY Delete
✅ Block data in RocksDB (for blocks marked BLOCK_PRUNE_ELIGIBLE)
✅ Undo data in flat files (rev*.dat, for eligible blocks)

### What Pruning MUST NEVER Delete
❌ Any block on the active chain
❌ Any block within 288 blocks of tip (MIN_BLOCKS_TO_KEEP)
❌ Genesis block (height == 0, implicit protection)
❌ Any block without BLOCK_HAVE_UNDO flag
❌ CBlockIndex metadata (positions must persist even after data deletion)
❌ Block headers (topology must survive pruning)

### What Must Be True After Restart
✅ CBlockIndex undo positions restored from ChainDB
✅ Blocks without BLOCK_HAVE_DATA flag cannot be read
✅ Blocks without BLOCK_HAVE_UNDO flag cannot be reverted
✅ PruneStats.lowest_block_height matches actual pruned state
✅ Active chain fully traversable (all headers present)

### What Reorg Guarantees Remain
✅ Can reorg up to 288 blocks deep (MIN_BLOCKS_TO_KEEP protection)
✅ Cannot reorg past pruned blocks (undo data required for disconnect)
✅ Reorg safety is monotonic (pruning never weakens guarantees)

---

## 2. Data Ownership Table (Source of Truth)

| Data Type | Owner | Persisted Where | Prunable | Restart Behavior |
|-----------|-------|-----------------|----------|------------------|
| **Block data** | ChainDB | RocksDB `idx_blocks_` | ✅ Yes | Deleted via ChainDB::deleteBlock() |
| **Undo data** | BlockStorage | Flat files `rev*.dat` | ✅ Yes | Zeroed via BlockStorage::pruneUndoDataFromCBlockIndex() |
| **CBlockIndex undo positions** | ChainDB | Serialized in block index | ❌ Never | Restored on startup, cleared on prune |
| **Block headers** | ChainDB | `idx_headers_` column family | ❌ Never | Always available for header-first sync |
| **Height index** | ChainDB | `idx_height_` column family | ❌ Never | Topology always traversable |

**Critical:** After pruning, CBlockIndex must have:
- `undo_file = 0` (indicates no undo data)
- `undo_pos = 0`
- `undo_size = 0`
- `status & BLOCK_HAVE_DATA` cleared
- `status & BLOCK_HAVE_UNDO` cleared

---

## 3. Current Architecture (Hybrid Model)

### Storage Split
- **Blocks:** RocksDB (ChainDB) - full block serialization
- **Undo:** Flat files (BlockStorage) - rev00000.dat, rev00001.dat, etc.

### Why This Matters for Pruning
1. **Block deletion:** ChainDB::deleteBlock() removes from RocksDB
2. **Undo deletion:** BlockStorage::pruneUndoDataFromCBlockIndex() zeros flat file
3. **Both must succeed** or pruning is incomplete

### CBlockIndex Disk Position Fields (Already Added)

```cpp
class CBlockIndex {
    // Block data positions (future - not used in P.2)
    uint32_t file_number{0};  // For future flat-file block storage
    uint32_t data_pos{0};
    uint32_t data_size{0};

    // Undo data positions (USED IN P.2 - already wired up!)
    uint32_t undo_file{0};    // rev*.dat file number (0 = no undo)
    uint32_t undo_pos{0};     // Offset in undo file
    uint32_t undo_size{0};    // Size of undo data
};
```

**Status:** undo_file/undo_pos/undo_size populated by parallel_block_validator.cpp ✅

---

## 4. Exact Serialization Changes Needed

### File: `src/storage/chain_db.cpp`

**Current CBlockIndex Serialization (Needs Extension):**

Locate the block index serialization function (likely in `putBlockIndex` or similar).

**Add These Fields to Serialization (3 new uint32_t fields):**

```cpp
// Existing fields (already serialized):
// - hash (32 bytes)
// - prev_hash (32 bytes)
// - height (4 bytes)
// - chainwork (variable, hex string)
// - status (4 bytes)
// - timestamp, bits, nonce, etc.

// NEW FIELDS TO ADD (12 bytes total):
writer.writeUint32(pindex->undo_file);   // 4 bytes
writer.writeUint32(pindex->undo_pos);    // 4 bytes
writer.writeUint32(pindex->undo_size);   // 4 bytes
```

**Deserialization (on restart):**

```cpp
pindex->undo_file = reader.readUint32();
pindex->undo_pos = reader.readUint32();
pindex->undo_size = reader.readUint32();
```

**Version Bump:** NO VERSION BUMP NEEDED if you can detect missing fields gracefully:
```cpp
// If old data (before P.2), default to zero
if (data_available) {
    pindex->undo_file = reader.readUint32();
    pindex->undo_pos = reader.readUint32();
    pindex->undo_size = reader.readUint32();
} else {
    // Old format - no undo positions stored
    pindex->undo_file = 0;
    pindex->undo_pos = 0;
    pindex->undo_size = 0;
}
```

**Restart Load Order:**
1. Load CBlockIndex from ChainDB
2. Populate undo_file/undo_pos/undo_size from serialized data
3. If undo_file == 0, block has no undo data (pruned or never connected)

---

## 5. ChainDB::deleteBlock() Implementation

### File: `include/storage/chain_db.h`

**Add Method Declaration:**

```cpp
// Phase P.2: Delete block data from RocksDB
// Used by pruning to free disk space
// Does NOT delete headers or block index - only the full block body
Status deleteBlock(const ChainWriteToken& token, const uint256& hash, rocksdb::WriteBatch* wb = nullptr);
```

### File: `src/storage/chain_db.cpp`

**Add Implementation:**

```cpp
Status ChainDB::deleteBlock(const ChainWriteToken& token, const uint256& hash, rocksdb::WriteBatch* wb) {
    if (!db_) return Status::Internal;

    // token validates authorization (compile-time check)
    (void)token;

    auto key = makeBlockKey(hash);

    if (wb) {
        wb->Delete(cf_[idx_blocks_].get(), key);
        return Status::Ok;
    } else {
        auto status = db_->Delete(rocksdb::WriteOptions(), cf_[idx_blocks_].get(), key);
        return convertRocksDBStatus(status);
    }
}
```

**Safety:** This only deletes block body, not headers or index. Headers remain for topology.

---

## 6. BlockStorage Header Declarations

### File: `include/storage/block_storage.h`

**Add After Existing pruneBlockData() Declaration:**

```cpp
// Phase P.2: Prune undo data using CBlockIndex disk positions
// Current architecture: Blocks in RocksDB, Undo in flat files
// This function only prunes the undo data (flat file zero-out)
// Block data deletion from RocksDB handled by ChainDB::deleteBlock()
//
// Returns:
//   Status::Ok on success (or if no undo data present)
//   Status::Io on file operation failure
Status pruneUndoDataFromCBlockIndex(const uint256& block_hash,
                                    uint32_t undo_file_num,
                                    uint32_t undo_offset,
                                    uint32_t undo_data_size,
                                    uint32_t height);
```

---

## 7. PruneService::pruneToHeight() Implementation

### File: `src/daemon/services/prune_service.cpp`

**Add After Existing Methods:**

```cpp
PruneResult PruneService::pruneToHeight(uint32_t target_height) {
    PruneResult result;

    // Get chainstate service for access to ChainManager
    if (!ctx_ || !ctx_->chainstate) {
        result.errors.push_back("Chainstate service not available");
        result.blocks_failed = 1;
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx_->chainstate);
    if (!chainstate) {
        result.errors.push_back("Failed to cast chainstate service");
        result.blocks_failed = 1;
        return result;
    }

    ChainManager& chain_manager = chainstate->chainManager();
    uint32_t active_height = chain_manager.GetHeight();

    // Safety check: enforce MIN_BLOCKS_TO_KEEP (288)
    if (target_height > active_height - MIN_BLOCKS_TO_KEEP) {
        result.errors.push_back("Target height too close to tip (MIN_BLOCKS_TO_KEEP = 288)");
        result.blocks_failed = 1;
        return result;
    }

    // Iterate all blocks in g_block_index
    for (auto& [hash, block_index_ptr] : g_block_index) {
        CBlockIndex* pindex = block_index_ptr.get();

        // Skip blocks above target height
        if (pindex->height >= target_height) {
            continue;
        }

        result.blocks_attempted++;

        // Check if block is marked BLOCK_PRUNE_ELIGIBLE
        if (!(pindex->status & BLOCK_PRUNE_ELIGIBLE)) {
            continue;  // Not eligible, skip
        }

        // Re-validate eligibility (safety check)
        if (!chain_manager.ComputePruneEligibility(pindex)) {
            continue;  // No longer eligible
        }

        // PRUNE UNDO DATA (flat file)
        if (pindex->undo_file != 0 && pindex->undo_size != 0) {
            BlockStorage* block_storage = chain_manager.GetBlockStorage();
            if (block_storage) {
                auto undo_status = block_storage->pruneUndoDataFromCBlockIndex(
                    pindex->hash,
                    pindex->undo_file,
                    pindex->undo_pos,
                    pindex->undo_size,
                    pindex->height
                );

                if (undo_status != Status::Ok) {
                    result.errors.push_back("Failed to prune undo for block " +
                                          pindex->hash.GetHex().substr(0, 16));
                    result.blocks_failed++;
                    continue;
                }

                result.bytes_recovered += (8 + pindex->undo_size);

                // Clear undo position in CBlockIndex
                pindex->undo_file = 0;
                pindex->undo_pos = 0;
                pindex->undo_size = 0;
                pindex->status &= ~BLOCK_HAVE_UNDO;
            }
        }

        // PRUNE BLOCK DATA (RocksDB)
        if (pindex->status & BLOCK_HAVE_DATA) {
            ChainDB* chain_db = chain_manager.GetChainDB();
            if (chain_db) {
                ChainWriteToken token = chain_db->getWriteToken();
                auto block_status = chain_db->deleteBlock(token, pindex->hash);

                if (block_status != Status::Ok) {
                    result.errors.push_back("Failed to delete block data for " +
                                          pindex->hash.GetHex().substr(0, 16));
                    result.blocks_failed++;
                    continue;
                }

                // Estimate block size (not tracked in current architecture)
                // Use average of ~1KB per block
                result.bytes_recovered += 1024;

                // Clear BLOCK_HAVE_DATA flag
                pindex->status &= ~BLOCK_HAVE_DATA;
            }
        }

        // Persist updated CBlockIndex to ChainDB
        // (flags cleared, undo positions zeroed)
        // TODO: Add ChainDB::updateBlockIndex() method if not exists

        result.blocks_pruned++;

        // Log progress every 100 blocks
        if (result.blocks_pruned % 100 == 0) {
            g_logger.info("Phase P.2: Pruned " + std::to_string(result.blocks_pruned) +
                         " blocks, freed " + std::to_string(result.bytes_recovered) + " bytes");
        }
    }

    // Update PruneStats
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.blocks_pruned += result.blocks_pruned;
        stats_.bytes_pruned += result.bytes_recovered;
        stats_.lowest_block_height = target_height;
        stats_.is_pruned = (result.blocks_pruned > 0);
    }

    g_logger.info("Phase P.2: Pruning complete - " +
                 std::to_string(result.blocks_pruned) + " blocks pruned, " +
                 std::to_string(result.bytes_recovered) + " bytes freed");

    return result;
}
```

**Critical Missing Piece:** ChainManager needs getters for BlockStorage and ChainDB:

```cpp
// Add to include/consensus/chain_manager.h:
BlockStorage* GetBlockStorage() const { return block_storage_; }
ChainDB* GetChainDB() const { return chain_db_; }
```

---

## 8. RPC Commands

### File: `src/rpc/methods_blockchain_context.cpp`

**Add pruneblockchain RPC:**

```cpp
din::Json rpc_pruneblockchain(const ExecutionContext& ctx, const din::Json& params) {
    auto prune_service = std::dynamic_pointer_cast<PruneService>(ctx.daemon->prune_service);

    if (!prune_service) {
        din::Json error;
        error["error"] = "Prune service not available";
        return error;
    }

    if (!prune_service->isEnabled()) {
        din::Json error;
        error["error"] = "Pruning is not enabled";
        return error;
    }

    // Get target height from params
    if (!params.contains("height") || !params["height"].is_number()) {
        din::Json error;
        error["error"] = "Missing or invalid 'height' parameter";
        return error;
    }

    uint32_t target_height = params["height"].get<uint32_t>();

    // Execute pruning
    PruneResult result = prune_service->pruneToHeight(target_height);

    // Build response
    din::Json response;
    response["blocks_pruned"] = result.blocks_pruned;
    response["bytes_recovered"] = result.bytes_recovered;
    response["lowest_block_height"] = prune_service->getStats().lowest_block_height;
    response["success"] = result.success();

    if (!result.success()) {
        din::Json errors = din::Json::array();
        for (const auto& err : result.errors) {
            errors.push_back(err);
        }
        response["errors"] = errors;
    }

    return response;
}
```

**Extend getblockchaininfo:**

```cpp
// Add to existing rpc_getblockchaininfo implementation:
auto prune_service = std::dynamic_pointer_cast<PruneService>(ctx.daemon->prune_service);
if (prune_service) {
    result["pruned"] = prune_service->isEnabled();
    if (prune_service->isEnabled()) {
        auto stats = prune_service->getStats();
        result["pruneheight"] = stats.lowest_block_height;
        result["blocks_pruned"] = stats.blocks_pruned;
        result["bytes_pruned"] = stats.bytes_pruned;
    }
}
```

**Register RPC (src/rpc/rpc_registration.cpp):**

```cpp
RegisterRPCMethod("pruneblockchain", rpc_pruneblockchain);
```

---

## 9. Test Matrix (Minimum Coverage)

### Test 1: Simple Prune
```
1. Create 400-block chain
2. Mark blocks 0-111 as BLOCK_PRUNE_ELIGIBLE (400 - 288 = 112)
3. Call pruneToHeight(112)
4. Verify:
   - Blocks 0-111 have no BLOCK_HAVE_DATA
   - Blocks 0-111 have no BLOCK_HAVE_UNDO
   - Blocks 112-399 still have data
   - Active chain traversable
```

### Test 2: Restart After Prune
```
1. Prune 100 blocks
2. Shutdown node
3. Restart node
4. Verify:
   - PruneStats.lowest_block_height = 100
   - CBlockIndex positions restored (undo_file = 0 for pruned blocks)
   - Active chain still valid
```

### Test 3: Cannot Prune Active Chain
```
1. Attempt to prune block on active chain
2. Verify:
   - ComputePruneEligibility returns false
   - Block not pruned
   - Error logged
```

### Test 4: Cannot Prune Recent Blocks
```
1. Attempt to prune block at height (tip - 200)
2. Verify:
   - Rejected (< MIN_BLOCKS_TO_KEEP)
   - Error message clear
```

### Test 5: RPC Contract
```
1. Call pruneblockchain with invalid height
2. Verify error response
3. Call with valid height
4. Verify success response with stats
5. Call getblockchaininfo
6. Verify pruned=true, pruneheight set
```

---

## 10. Execution Checklist (Follow in Order)

### Step 1: ChainDB Integration
- [ ] Add ChainDB::deleteBlock() declaration to header
- [ ] Implement ChainDB::deleteBlock() in .cpp
- [ ] Add GetBlockStorage() and GetChainDB() getters to ChainManager

### Step 2: Serialization
- [ ] Locate CBlockIndex serialization in chain_db.cpp
- [ ] Add undo_file/undo_pos/undo_size to serialization
- [ ] Add backward-compat fallback (default to 0 if not present)
- [ ] Test restart after change (verify no crashes)

### Step 3: BlockStorage Headers
- [ ] Add pruneUndoDataFromCBlockIndex() declaration to block_storage.h
- [ ] Verify zeroOutFileRegion() is declared as private helper

### Step 4: PruneService
- [ ] Implement pruneToHeight() following pseudocode above
- [ ] Add progress logging (every 100 blocks)
- [ ] Update PruneStats after completion

### Step 5: RPC Layer
- [ ] Implement rpc_pruneblockchain
- [ ] Extend rpc_getblockchaininfo
- [ ] Register pruneblockchain in rpc_registration.cpp

### Step 6: Testing
- [ ] Write test_pruning_integration.cpp (5 tests from matrix)
- [ ] Add to CMakeLists.txt
- [ ] Run all tests
- [ ] Fix any failures

### Step 7: Documentation
- [ ] Create PHASE_P2_PHYSICAL_PRUNING_LOCK.md
- [ ] Document what was implemented
- [ ] Document integration with P.1
- [ ] Lock invariants and forbidden modifications

---

## 11. Known Gaps to Address

### Missing: ChainDB::updateBlockIndex()
**Problem:** After pruning, need to persist updated CBlockIndex (cleared flags, zeroed positions)

**Solution:** Add method to ChainDB:
```cpp
Status updateBlockIndex(const ChainWriteToken& token, const uint256& hash,
                       uint32_t status, uint32_t undo_file, uint32_t undo_pos, uint32_t undo_size);
```

Call after clearing flags in pruneToHeight().

### Missing: Block Size Tracking
**Problem:** Don't know exact bytes freed when deleting from RocksDB

**Temporary:** Use estimate (~1KB per block)
**Future:** Track block size in CBlockIndex (data_size field already exists but not populated)

---

## 12. Success Criteria (Phase P.2 Complete)

✅ Can prune blocks to specific height via RPC
✅ Blocks pruned: BLOCK_HAVE_DATA and BLOCK_HAVE_UNDO flags cleared
✅ Undo data zeroed in flat files
✅ Block data deleted from RocksDB
✅ Restart recovers pruned state (stats + flags)
✅ Cannot prune active chain or recent blocks
✅ RPC returns clear success/error
✅ All tests pass
✅ Lock document created

---

## 13. Estimated Time (Next Session)

- ChainDB integration: 30 min
- Serialization changes: 45 min
- PruneService implementation: 60 min
- RPC commands: 30 min
- Testing: 45 min
- Documentation: 30 min

**Total: ~4 hours of focused execution**

---

## 14. Final Reminder

**This is mechanical work. No architectural decisions remain.**

If you encounter any uncertainty:
1. Check this guide first
2. Check Phase P.1 lock document
3. Ask, don't assume

The hard part is done. This is just connecting pieces correctly.
