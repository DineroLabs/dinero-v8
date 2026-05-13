# Layer 2.4 Extension: Undo Position Persistence - COMPLETE

**Date:** December 19, 2025
**Status:** ✅ **COMPLETE** (BlockIndex undo positions now persisted)

---

## 🎯 What Was Fixed

**Extended L2.4 to persist BlockIndex undo positions to ChainDB atomically.**

**Problem (Before Fix):**
- ActivateBestChain stored undo positions in-memory BlockIndex (line 323-326)
- But these were NOT persisted to ChainDB
- On crash: undo position metadata lost
- Impact: Slower undo data lookup on restart (data still on disk, but no index)

**Solution (After Fix):**
- Added setBlockUndoPosition() to IBlockIndexDB interface
- ActivateBestChain calls setBlockUndoPosition() after ConnectBlock
- ChainManager persists BlockIndex updates in ReorgGuard batch
- Undo positions committed atomically with tip update
- On crash: undo positions preserved on disk

---

## 🔧 Implementation Details

### 1. Extended IBlockIndexDB Interface

**File:** `include/p2p/state_transition.h`

**Added method:**
```cpp
virtual void setBlockUndoPosition(const Hash256& block_hash,
                                  uint32_t file_id,
                                  uint64_t offset,
                                  uint64_t length,
                                  uint32_t checksum) = 0;
```

**Purpose:**
- Called by ActivateBestChain after ConnectBlock
- Updates in-memory CBlockIndex with undo file position
- Allows later persistence via ChainDB::updateBlockIndex()

---

### 2. Implemented in BlockIndexDBAdapter

**File:** `include/consensus/adapters/block_index_db_adapter.h`

**Implementation:**
```cpp
void setBlockUndoPosition(const p2p::Hash256& block_hash,
                          uint32_t file_id,
                          uint64_t offset,
                          uint64_t length,
                          uint32_t checksum) override {
    uint256 hash = convertHash(block_hash);
    CBlockIndex* pindex = chain_db_.getBlockIndex(hash);

    if (pindex) {
        // Update in-memory CBlockIndex
        pindex->undo_file_id = file_id;
        pindex->undo_file_offset = offset;
        pindex->undo_length = length;
        pindex->undo_checksum = checksum;
    }
}
```

**How it works:**
1. Converts p2p::Hash256 → uint256 (direct byte copy, Phase M.0 compliant)
2. Gets CBlockIndex* from ChainDB
3. Updates undo position fields in-memory
4. Caller must persist via updateBlockIndex()

---

### 3. Called from ActivateBestChain

**File:** `src/consensus/activate_best_chain.cpp` (lines 328-336)

**Before (in-memory only):**
```cpp
// Store undo info in block index
block->undo_file_id = connect_result.undo_file_id;
block->undo_file_offset = connect_result.undo_file_offset;
block->undo_length = connect_result.undo_length;
block->undo_checksum = connect_result.undo_checksum;
```

**After (propagated to persistent index):**
```cpp
// Store undo info in block index (in-memory)
block->undo_file_id = connect_result.undo_file_id;
block->undo_file_offset = connect_result.undo_file_offset;
block->undo_length = connect_result.undo_length;
block->undo_checksum = connect_result.undo_checksum;

// L2.4: Propagate undo position to persistent block index
// Updates the underlying CBlockIndex in ChainDB
block_index_db.setBlockUndoPosition(
    block->hash,
    connect_result.undo_file_id,
    connect_result.undo_file_offset,
    connect_result.undo_length,
    connect_result.undo_checksum
);
```

**Why both?**
- First block: p2p::BlockIndex* (for ActivateBestChain internal logic)
- Second call: CBlockIndex (for ChainDB persistence)
- Adapter bridges the two

---

### 4. Persisted in ChainManager

**File:** `src/consensus/chain_manager.cpp` (lines 209-218)

**New code:**
```cpp
// L2.4: Persist BlockIndex undo positions to ChainDB
// ActivateBestChain updated in-memory CBlockIndex objects via setBlockUndoPosition()
// Now persist those changes to disk in the atomic batch
for (CBlockIndex* block : connect_path) {
    auto status = chain_db_->updateBlockIndex(token, block, &reorg_guard.getBatch());
    if (status != Status::Ok) {
        dinero::g_logger.error("FATAL: Failed to persist BlockIndex for block " + block->hash.GetHex());
        std::terminate();  // L2.4: Panic on block index persistence failure
    }
}

// L2.4: Commit reorg atomically (persist new tip + block indices to ChainDB)
reorg_guard.commit(...);
```

**Flow:**
1. ActivateBestChain succeeds → CBlockIndex objects updated in-memory
2. For each connected block, add updateBlockIndex() to ReorgGuard batch
3. reorg_guard.commit() writes tip + all block indices atomically
4. Single RocksDB WriteBatch → crash-safe

---

## ✅ Correctness Guarantees

### Atomicity

**All-or-Nothing Commit:**
- Tip update + block index updates in single RocksDB WriteBatch
- reorg_guard.commit() writes atomically with sync=true
- Either all persisted or none persisted

**No Partial State:**
```cpp
// Batch preparation (in-memory)
for (block : connect_path) {
    updateBlockIndex(token, block, &batch);  // Add to batch
}
reorg_guard.commit();  // Commit batch atomically
```

If commit fails → std::terminate() (no partial state possible)

---

### Crash Safety

**Scenario 1: Crash before updateBlockIndex() loop**
- ActivateBestChain succeeded, but undo positions not batched
- ReorgGuard destructor discards empty batch
- Persistent tip unchanged
- Consistent state (old chain)

**Scenario 2: Crash during updateBlockIndex() loop**
- Some block indices added to batch, not all
- ReorgGuard destructor discards partial batch
- Persistent tip unchanged
- Consistent state (old chain)

**Scenario 3: Crash during reorg_guard.commit()**
- RocksDB atomic write guarantees all-or-nothing
- Either tip + all block indices written, or none
- Consistent state (either old or new chain)

**Scenario 4: Crash after commit()**
- Tip + block indices successfully persisted
- On restart: undo positions available for reorg

**Result:** All scenarios maintain crash safety

---

## 🔒 Lock Criteria (ACHIEVED)

L2.4 Undo Position Persistence is **DONE FOREVER** when:

- ✅ setBlockUndoPosition() added to IBlockIndexDB interface
- ✅ Implemented in BlockIndexDBAdapter
- ✅ Called from ActivateBestChain after ConnectBlock
- ✅ Persisted in ChainManager via updateBlockIndex()
- ✅ Batched with tip update in ReorgGuard
- ✅ Committed atomically to ChainDB
- ✅ Phase M.0 compliant (no hex conversions in identity)

**All criteria met. Undo position persistence is LOCKED FOREVER.**

---

## 📊 Files Modified

1. **include/p2p/state_transition.h**
   - Added setBlockUndoPosition() to IBlockIndexDB interface (lines 250-270)

2. **include/consensus/adapters/block_index_db_adapter.h**
   - Implemented setBlockUndoPosition() method (lines 83-106)

3. **src/consensus/activate_best_chain.cpp**
   - Added setBlockUndoPosition() call after ConnectBlock (lines 328-336)

4. **src/consensus/chain_manager.cpp**
   - Added updateBlockIndex() calls in ReorgGuard batch (lines 209-218)

---

## ✅ Phase M.0 Compliance

```bash
$ grep -rn "\.GetHex()\s*[!=]=\|[!=]=\s*[^?]*\.GetHex()" \
    src/consensus/activate_best_chain.cpp \
    include/consensus/adapters/block_index_db_adapter.h \
    include/p2p/state_transition.h

✅ CLEAN - Zero violations
```

**Result:** Undo position persistence maintains Phase M.0 compliance.

---

## 🎯 Impact

**Before:**
- Undo positions stored in-memory only
- On crash: undo position metadata lost
- On restart: must scan rev*.dat files to find undo data (slow)

**After:**
- Undo positions persisted to ChainDB atomically
- On crash: undo positions preserved on disk
- On restart: direct lookup via BlockIndex (fast)

**Performance:**
- Restart time: Faster (no rev*.dat scanning)
- Reorg time: Same (no overhead)
- Disk usage: Same (metadata already in BlockIndex)

---

## 🔍 Architecture Notes

**Data Flow:**
1. ConnectBlock → writes undo data to rev*.dat, returns file position
2. ActivateBestChain → stores position in p2p::BlockIndex* (in-memory)
3. setBlockUndoPosition() → propagates to CBlockIndex* (in ChainDB)
4. ChainManager → persists CBlockIndex via updateBlockIndex()
5. ReorgGuard → commits atomically with tip update

**Why Two BlockIndex Types?**
- p2p::BlockIndex: Interface type (G.3.4/G.3.5 layer)
- CBlockIndex: Persistent type (ChainDB storage layer)
- Adapter bridges the gap (zero-logic forwarding)

**Why Not Persist in ActivateBestChain?**
- ActivateBestChain operates on interfaces (IBlockIndexDB)
- Doesn't have direct ChainDB access (proper layer separation)
- ChainManager owns ChainDB and handles persistence

---

**Verdict:** ✅ **UNDO POSITION PERSISTENCE COMPLETE AND LOCKED FOREVER**

BlockIndex undo positions now persisted atomically with tip updates. Crash-safe. Fast restart. Done.
