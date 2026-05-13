# Phase G.3.4: State Transition Layer - Design Document

**Status:** Design stub - NOT IMPLEMENTED
**Purpose:** Architectural contract definition BEFORE implementation
**Created:** 2024-12-17
**WARNING:** This is the one-way door from pure evaluation → state mutation

---

## Executive Summary

Phase G.3.4 implements the **State Transition Layer** - the component that **applies consequences** to the chain state after G.3.3 has determined truth.

**Mental Model:**
- G.3.3 asks: "Is this valid?"
- G.3.4 does: "Make it real."

This is Bitcoin Core's `ConnectBlock()` / `DisconnectBlock()` equivalent.

---

## 1. What is the exact input to ConnectBlock?

### Primary Input
```cpp
struct ConnectBlockInput {
    // The block to connect
    Block block;

    // Current chain tip (parent must match)
    BlockIndex* parent_index;

    // Read-write UTXO set interface (NOT read-only like G.3.3)
    IChainState& chainstate;

    // Consensus parameters
    const ConsensusParams& params;

    // Validation mode flags
    ValidationFlags flags;  // e.g., skip_script_checks during IBD with assumevalid
};
```

### Preconditions (MUST be enforced by caller)
1. **Block has been structurally validated (G.3.2)** ✅
2. **Block has been consensus validated (G.3.3)** ✅
3. **Parent block is in active chain** ✅
4. **Chainstate is locked for writes** ✅
5. **No conflicting block at same height** ✅

### Validation Flags
```cpp
enum ValidationFlags {
    VALIDATE_FULL = 0,           // Full validation (mainnet, testnet)
    SKIP_SCRIPTS_ASSUMEVALID,    // Skip scripts below assumevalid height (IBD speedup)
    SKIP_UTXO_CHECKS,            // Dangerous - only for reindex from trusted source
};
```

---

## 2. What is the exact output?

### Success Output
```cpp
struct ConnectBlockResult {
    bool success;

    // Updated block index entry
    BlockIndex* new_tip;

    // Undo data for reorg safety
    BlockUndo undo_data;

    // Statistics
    struct Stats {
        uint64_t inputs_spent;
        uint64_t outputs_created;
        uint64_t fees_collected;
        uint64_t subsidy_claimed;
        size_t tx_count;
    } stats;

    // Error info (if !success)
    std::string error;
    BlockRejectReason reject_reason;
};
```

### Side Effects (Critical - must be atomic)
1. **UTXO Set Updated:**
   - All transaction inputs marked as spent
   - All transaction outputs added as new UTXOs

2. **Block Index Updated:**
   - `chainwork` accumulated
   - `status` updated to `BLOCK_VALID_TRANSACTIONS`
   - `height` recorded

3. **Undo Data Persisted:**
   - Written to `rev*.dat` BEFORE declaring success
   - Contains all spent UTXOs (needed for disconnect)

4. **Chainstate Metadata Updated:**
   - Best block hash
   - Best height
   - Total chainwork

---

## 3. What undo data must be generated?

### BlockUndo Structure
```cpp
struct BlockUndo {
    // For each transaction in block (except coinbase)
    std::vector<TxUndo> tx_undo;

    struct TxUndo {
        // For each input in transaction
        std::vector<CTxOut> prev_outs;
    };
};
```

### Why Undo Data is Critical
- **Reorg Safety:** Must be able to disconnect blocks to switch to competing chain
- **Atomicity:** Write undo BEFORE marking block as connected
- **Persistence:** Undo data must survive node restart

### Undo Data Lifecycle
1. **During ConnectBlock:**
   - For each input: Record the TxOut being spent
   - Accumulate into `BlockUndo` structure

2. **Before Success:**
   - Serialize `BlockUndo`
   - Write to `rev*.dat` file
   - Flush to disk
   - Checksum verification

3. **During DisconnectBlock:**
   - Read `BlockUndo` from disk
   - For each tx (reverse order):
     - Remove outputs from UTXO set
     - Restore inputs to UTXO set using undo data

---

## 4. What must be atomic?

### ⚠️ CRITICAL: There Is NO Single Atomic Boundary

**Bitcoin Core Reality:**
- Filesystem writes (undo to `rev*.dat`) and database writes (UTXO/index to RocksDB) **cannot be made atomic together**
- This is a fundamental durability constraint, not an implementation detail

### Two-Phase Durability Model (Explicit)

**Phase A: Undo Durability (Filesystem)**
```
1. Generate BlockUndo structure in memory
2. Serialize to temporary file: rev{nFile}.dat.tmp
3. fsync() temporary file
4. Atomic rename: rev{nFile}.dat.tmp → rev{nFile}.dat
5. Checkpoint: Undo data is now durable (possibly orphaned)
```

**Phase B: State Mutation (Database Batch)**
```
BEGIN RocksDB WriteBatch
  1. Acquire exclusive chainstate lock (cs_main equivalent)
  2. Validate parent connection (double-check)
  3. For each transaction:
     a. Mark inputs as spent (remove from UTXO set)
     b. Add outputs to UTXO set
  4. Update block index:
     - Set status = BLOCK_VALID_TRANSACTIONS
     - Accumulate chainwork
     - Record height
  5. Update chainstate metadata:
     - best_block_hash
     - best_height
COMMIT RocksDB batch (atomic within database)
```

### Recovery Rules (Explicit Decision Tree)

**Case 1: Undo exists, block NOT in index**
- **Reason:** Phase A succeeded, Phase B failed
- **Action:** Delete orphaned undo file (safe)
- **Rationale:** Block was never connected, undo is garbage

**Case 2: Block in index, undo missing**
- **Reason:** Filesystem corruption or manual deletion
- **Action:** FATAL ERROR → Require reindex
- **Rationale:** Cannot safely reorg without undo

**Case 3: RocksDB batch fails (mid-commit)**
- **Reason:** Database corruption or crash during commit
- **Action:** RocksDB rollback (automatic)
- **Rationale:** Batch atomicity ensures UTXO set is consistent
- **Result:** Orphaned undo file remains (Case 1 cleanup on next startup)

**Case 4: Crash between Phase A and Phase B**
- **Reason:** Node crashed after undo written, before DB commit
- **Action:** On restart, detect orphaned undo (Case 1) and delete
- **Rationale:** Block was never connected

### Concurrency Model (Mandatory)

**ConnectBlock MUST be single-threaded:**
- Acquire exclusive chainstate lock (`cs_main` equivalent) before entry
- NO concurrent ConnectBlock or DisconnectBlock calls
- Even speculative parallel validation must serialize final commit
- **Rationale:** UTXO set is shared mutable state - concurrent writes = corruption

### Database Transaction Semantics

**RocksDB:**
- WriteBatch ensures atomic commit of all UTXO updates
- Either ALL changes apply or NONE apply
- No intermediate states visible to readers

**Filesystem:**
- Undo writes use atomic rename (POSIX guarantee)
- fsync() before rename ensures durability
- Orphaned undo files are safe (cleaned up on startup)

---

## 4.5. State Commit Contract (Precise Specification)

### What Exactly Is In The RocksDB Batch?

**CRITICAL:** All state mutations MUST be in a single RocksDB WriteBatch to prevent partial state.

**Single WriteBatch MUST contain (atomic commit):**
```
1. UTXO Set Updates:
   - DELETE keys for all spent inputs: "utxo:{txid}:{index}"
   - PUT keys for all created outputs: "utxo:{txid}:{index}" → TxOut{value, scriptPubKey}

2. Chainstate Metadata:
   - PUT "chainstate:best_block_hash" → block.hash
   - PUT "chainstate:best_height" → block.height
   - PUT "chainstate:total_chainwork" → parent.chainwork + block.work

3. Block Index Entry:
   - PUT "blockindex:{block.hash}:status" → BLOCK_CONNECTED
   - PUT "blockindex:{block.hash}:height" → block.height
   - PUT "blockindex:{block.hash}:chainwork" → accumulated_work

4. Undo Mapping Entry:
   - PUT "undo:{block.hash}" → {file_id, offset, length, checksum}
```

**If RocksDB batch commit fails:**
- Block is NOT considered connected (regardless of undo existence)
- All keys above remain unchanged (batch atomicity)
- Orphaned undo file remains on disk (cleaned on next startup)

**Failure Modes Prevented:**
- ❌ UTXO updated but chainstate tip not updated
- ❌ Block index updated but UTXO not updated (worst - money creation)
- ❌ Chainstate tip updated but UTXO missing (spend non-existent coins)

### Block Validity Flags (Minimal Ladder)

**Bitcoin Core uses a validity ladder to prevent half-valid blocks. We use a simplified version:**

```cpp
enum BlockStatus {
    BLOCK_VALID_HEADER      = 0x01,  // Header only, not validated
    BLOCK_HAVE_DATA         = 0x02,  // Block data stored to disk
    BLOCK_HAVE_UNDO         = 0x04,  // Undo data written (Phase A complete)
    BLOCK_CONNECTED         = 0x08,  // UTXO + metadata committed (Phase B complete)
    BLOCK_FAILED_VALID      = 0x10,  // Consensus validation failed (G.3.3)
    BLOCK_FAILED_CHILD      = 0x20,  // Descendant failed validation
};
```

**Status Transition During ConnectBlock:**
```
BEFORE ConnectBlock:
  - status = BLOCK_VALID_HEADER | BLOCK_HAVE_DATA
  - OR status = BLOCK_HAVE_DATA (if header validation not tracked separately)

AFTER Phase A (undo written):
  - status |= BLOCK_HAVE_UNDO
  - (Not persisted to DB yet - only in memory)

AFTER Phase B (DB commit):
  - status |= BLOCK_CONNECTED
  - Persisted in single RocksDB batch with UTXO + metadata

AFTER ConnectBlock success:
  - status = BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO | BLOCK_CONNECTED
```

**Status Requirements for DisconnectBlock:**
```
PRECONDITION:
  - Block must have BLOCK_CONNECTED flag set
  - Block must have BLOCK_HAVE_UNDO flag set (undo exists)
  - If either missing → FATAL ERROR, cannot disconnect

AFTER DisconnectBlock:
  - Clear BLOCK_CONNECTED flag
  - Keep BLOCK_HAVE_UNDO flag (undo persists for re-connect)
  - Keep BLOCK_HAVE_DATA flag (block data persists)
```

**Permanent Failure Marking:**
```
If block fails G.3.3 validation:
  - Set BLOCK_FAILED_VALID flag
  - Never attempt to connect again
  - If block has descendants, mark them BLOCK_FAILED_CHILD
```

### Undo File Identity and Mapping

**Problem:** Without deterministic mapping, undo can become orphaned or corrupted silently.

**Solution: Explicit Undo Mapping (Stored in RocksDB)**
```cpp
struct UndoMapping {
    uint32_t file_id;       // Which rev*.dat file (e.g., 0 → rev00000.dat)
    uint64_t offset;        // Byte offset in file
    uint32_t length;        // Undo data length in bytes
    uint256  checksum;      // SHA256 of undo data (corruption detection)
};

// RocksDB key: "undo:{block_hash}" → UndoMapping
```

**Undo Write Process (Phase A):**
```
1. Serialize BlockUndo to buffer
2. Compute checksum = SHA256(buffer)
3. Append to current rev*.dat file at offset
4. fsync() file
5. Create UndoMapping{file_id, offset, length, checksum}
6. Store in memory (not persisted yet)
```

**Undo Persistence (Phase B - part of RocksDB batch):**
```
PUT "undo:{block.hash}" → UndoMapping{file_id, offset, length, checksum}
```

**Undo Load Process (DisconnectBlock):**
```
1. Load UndoMapping from DB: "undo:{block.hash}"
2. If not found → FATAL ERROR (cannot disconnect)
3. Open rev{file_id}.dat at offset
4. Read length bytes
5. Compute checksum' = SHA256(data)
6. If checksum' != checksum → FATAL ERROR (corruption detected)
7. Deserialize BlockUndo
8. Use for UTXO restoration
```

**Orphaned Undo Cleanup (Startup):**
```
For each entry in RocksDB "undo:*":
  - Load block_hash from key
  - Check if block exists in index
  - If NOT → delete undo mapping entry + mark file space as reclaimable
  - If block exists but NOT connected → delete undo mapping
```

### Pruning + Undo Retention Interaction

**Critical Question:** Can we disconnect a block if block data is pruned?

**Answer:** Depends on what DisconnectBlock requires.

**DisconnectBlock Requirements:**
```
ALWAYS required:
  ✅ Undo data (BlockUndo)
     - Contains TxOut for each spent input
     - Sufficient to restore UTXO set

SOMETIMES required:
  ⚠️ Block data (transactions)
     - Only if we need to re-verify something
     - NOT needed if we trust undo data is correct

NEVER required:
  ❌ UTXO set lookups
     - We're restoring UTXOs, not reading them
```

**DineroCoin Design Decision:**
```
DisconnectBlock requires ONLY undo data, NOT block data.

Rationale:
  - BlockUndo contains all spent TxOuts
  - We can restore UTXO set without re-reading transactions
  - Allows pruning block data while keeping undo for reorg safety

Therefore:
  - Block data (blk*.dat) can be pruned if depth > MIN_BLOCKS_TO_KEEP
  - Undo data (rev*.dat) MUST be kept if depth ≤ MIN_BLOCKS_TO_KEEP
  - Undo retention policy is INDEPENDENT of block data pruning
```

**If Deep Reorg Requires Pruned Block Data:**
```
Scenario: Reorg to height 1000, but block 1000 data is pruned
  - Undo data still exists (not pruned)
  - DisconnectBlock succeeds (only needs undo)
  - ConnectBlock on competing chain may need block data
  - If competing block data missing → refuse reorg OR re-download from peers

Design: DisconnectBlock never requires block data
        ConnectBlock always requires block data (already enforced by G.3.2/G.3.3)
```

### Fee Accounting Authority (Single Source of Truth)

**Problem:** Fee can be computed in G.3.3 or G.3.4 - divergence creates consensus bugs.

**Design Decision: G.3.3 is authoritative for fees**

**G.3.3 Extended Interface:**
```cpp
struct TxValidationResult {
    bool ok;
    uint64_t total_in;   // Sum of input values
    uint64_t total_out;  // Sum of output values
    uint64_t fee;        // total_in - total_out (0 for coinbase)
    std::string error;
};

TxValidationResult ConsensusValidator::validateTx(
    const Transaction& tx,
    const IUTXOSnapshot& utxo_view,
    const ConsensusParams& params
);
```

**G.3.4 Consumes Fee Results:**
```cpp
ConnectBlock(...) {
    uint64_t total_fees = 0;

    for (size_t i = 0; i < block.transactions.size(); i++) {
        // G.3.3 already validated this tx - reuse its fee calculation
        auto tx_result = cached_validation_results[i];  // From G.3.3
        assert(tx_result.ok);  // Must have passed G.3.3

        if (!block.transactions[i].isCoinbase()) {
            total_fees += tx_result.fee;
        }
    }

    // Validate coinbase claims correct subsidy + fees
    auto& coinbase = block.transactions[0];
    uint64_t coinbase_value = sumOutputs(coinbase);
    uint64_t max_allowed = GetBlockSubsidy(height, params) + total_fees;

    if (coinbase_value > max_allowed) {
        return Fail("Coinbase claims too much");
    }
}
```

**Critical Rule:**
- G.3.4 MUST NOT recompute fees independently
- G.3.4 MUST reuse fee values from G.3.3 validation results
- If recomputation is needed (e.g., no cache), use SAME helper function as G.3.3

**Alternative (if no result caching):**
```cpp
// Shared helper (used by both G.3.3 and G.3.4)
uint64_t ComputeTransactionFee(
    const Transaction& tx,
    const IUTXOSnapshot& utxo_view
) {
    if (tx.isCoinbase()) return 0;

    uint64_t total_in = 0;
    for (const auto& input : tx.inputs) {
        auto utxo = utxo_view.getUTXO(input.prevout);
        assert(utxo.has_value());  // Already validated by G.3.3
        total_in += utxo->value;
    }

    uint64_t total_out = 0;
    for (const auto& output : tx.outputs) {
        total_out += output.value;
    }

    return total_in - total_out;
}

// G.3.3 uses this helper
// G.3.4 uses this helper
// NO divergence possible
```

### Block-Level Coinbase Validation (G.3.3 Extension)

**Current Gap:** G.3.3 validates coinbase shape but not subsidy+fees correctness.

**Required Extension to G.3.3:**
```cpp
struct BlockValidationResult {
    bool ok;
    uint64_t total_fees;        // Sum of all non-coinbase tx fees
    uint64_t coinbase_value;    // Sum of coinbase outputs
    uint64_t subsidy;           // GetBlockSubsidy(height)
    std::string error;
};

BlockValidationResult ConsensusValidator::validateBlock(
    const Block& block,
    uint32_t height,
    const IUTXOSnapshot& utxo_view,
    const ConsensusParams& params
) {
    // 1. Validate all transactions
    std::vector<TxValidationResult> tx_results;
    for (const auto& tx : block.transactions) {
        auto result = validateTx(tx, utxo_view, params);
        if (!result.ok) {
            return Fail(result.error);
        }
        tx_results.push_back(result);
    }

    // 2. Accumulate fees
    uint64_t total_fees = 0;
    for (size_t i = 1; i < tx_results.size(); i++) {  // Skip coinbase
        total_fees += tx_results[i].fee;
    }

    // 3. Validate coinbase economics
    auto& coinbase = block.transactions[0];
    uint64_t coinbase_value = 0;
    for (const auto& out : coinbase.outputs) {
        coinbase_value += out.value;
    }

    uint64_t subsidy = GetBlockSubsidy(height, params);
    uint64_t max_allowed = subsidy + total_fees;

    if (coinbase_value > max_allowed) {
        return Fail("Coinbase output exceeds subsidy + fees");
    }

    return BlockValidationResult{
        .ok = true,
        .total_fees = total_fees,
        .coinbase_value = coinbase_value,
        .subsidy = subsidy,
        .error = ""
    };
}
```

**G.3.4 Role After G.3.3 Extension:**
```cpp
ConnectBlock(...) {
    // G.3.3 already validated block economics - just apply state changes
    // NO re-validation of subsidy or fees

    // Apply UTXO mutations
    for (const auto& tx : block.transactions) {
        applyTransaction(tx, utxo_set);  // Spend inputs, create outputs
    }

    // Update metadata
    updateChainstateTip(block.hash, block.height);

    // Statistics (optional, for logging)
    // Can reuse G.3.3 results if cached, or recompute for stats only
}
```

**Design Decision:**
- **G.3.3 is authoritative for all consensus validation (including block-level economics)**
- **G.3.4 applies state mutations only**
- **G.3.4 assumes G.3.3 has verified everything**

### Lock Order (Deadlock Prevention)

**Even though ConnectBlock is single-threaded today, future background tasks (mempool, wallet, compaction) can introduce deadlocks if lock order is not specified.**

**Explicit Lock Order (Must Be Respected):**
```
1. cs_chainstate (global chainstate lock)
   - Acquired: Before any chainstate reads/writes
   - Released: After RocksDB commit

2. RocksDB internal write lock (automatic)
   - Acquired: During WriteBatch::Write()
   - Released: After batch commit

3. Filesystem undo write (no explicit lock, but ordering matters)
   - Performed: BEFORE acquiring cs_chainstate (Phase A)
   - Rationale: Undo write is slow (I/O), don't hold cs_chainstate during I/O
```

**Correct Sequence:**
```
1. Generate undo data (no locks)
2. Write undo to filesystem (fsync + rename) (no locks)
3. ACQUIRE cs_chainstate
4. BEGIN RocksDB WriteBatch
5. Add UTXO updates to batch
6. Add chainstate metadata to batch
7. Add block index updates to batch
8. Add undo mapping to batch
9. COMMIT RocksDB batch (internal write lock acquired automatically)
10. RELEASE cs_chainstate
```

**Deadlock Prevention Rules:**
```
- NEVER hold cs_chainstate while doing filesystem I/O
- NEVER acquire cs_chainstate inside a RocksDB transaction
- ALWAYS write undo BEFORE acquiring cs_chainstate
- ALWAYS release cs_chainstate AFTER RocksDB commit
```

**Future-Proofing:**
```
When adding background tasks:
  - Mempool updates: Acquire cs_chainstate briefly, release before validation
  - Wallet scans: Read-only, can use UTXO snapshots
  - Compaction: RocksDB internal, no cs_chainstate needed
  - Flush threads: Coordinate with cs_chainstate via separate flush_mutex
```

### Startup Invariants (Complete)

**After loading block index and chainstate metadata, enforce these invariants:**

```
INVARIANT 1: Best-tip block must exist in index
  - Load best_block_hash from chainstate
  - Load BlockIndex entry for best_block_hash
  - If NOT found → FATAL: Corrupted chainstate → Reindex

INVARIANT 2: Best-tip block must be connected
  - Check block.status & BLOCK_CONNECTED != 0
  - If NOT set → WARNING: Incomplete connection → Roll back metadata to parent

INVARIANT 3: Best-tip block must have undo mapping
  - Load UndoMapping for best_block_hash
  - If NOT found → FATAL: Cannot reorg → Reindex

INVARIANT 4: Undo data must be readable and valid
  - Load undo data from rev*.dat using mapping
  - Compute checksum
  - If checksum mismatch → FATAL: Undo corruption → Reindex

INVARIANT 5: Orphaned undo must be cleaned
  - For each undo mapping entry:
    - If block NOT in index → Delete mapping + mark file space reclaimable
    - If block in index but NOT connected → Delete mapping

INVARIANT 6: Chainstate tip alignment
  - If block_index has higher tip than chainstate:
    - Log: "Rolling forward chainstate metadata"
    - Update chainstate.best_block_hash
    - Update chainstate.best_height
    - (Handles crash between index update and metadata update)
```

### All-or-Nothing Observable State

**After ConnectBlock completes (success or failure), observable state must be consistent:**

**SUCCESS:**
```
✅ Block has BLOCK_CONNECTED flag
✅ Best-tip metadata points to block
✅ UTXO set reflects all block transactions
✅ Undo mapping exists with valid checksum
✅ Block index chainwork updated
```

**FAILURE:**
```
✅ Block does NOT have BLOCK_CONNECTED flag
✅ Best-tip metadata unchanged (points to parent)
✅ UTXO set unchanged (RocksDB batch rolled back)
✅ Undo file may exist (orphaned) - cleaned on next startup
✅ Block may have BLOCK_FAILED_VALID flag set
```

**NO PARTIAL STATES:**
```
❌ Block connected but UTXO not updated
❌ UTXO updated but best-tip not updated
❌ Best-tip updated but block not marked connected
❌ Undo exists but not mapped in DB
```

---

## 5. What invariants must hold before and after?

### Precondition Invariants (BEFORE ConnectBlock)

1. **Chain Continuity:**
   ```
   block.prev_hash == parent_index->block_hash
   ```

2. **Height Monotonicity:**
   ```
   block.height == parent_index->height + 1
   ```

3. **UTXO Set Consistency:**
   ```
   All inputs referenced by block must exist in UTXO set
   (already verified by G.3.3, but double-check here)
   ```

4. **No Conflicting Blocks:**
   ```
   No other block at height (parent_index->height + 1) in active chain
   ```

### Postcondition Invariants (AFTER ConnectBlock)

1. **UTXO Set Updated:**
   ```
   For all tx in block:
     - All tx.inputs removed from UTXO set
     - All tx.outputs added to UTXO set
   ```

2. **Chainwork Accumulated:**
   ```
   new_tip->chainwork == parent_index->chainwork + block.work
   ```

3. **Undo Data Persisted:**
   ```
   BlockUndo exists on disk for block_hash
   ```

4. **Best Block Updated:**
   ```
   chainstate.best_block_hash == block.hash
   chainstate.best_height == block.height
   ```

5. **Money Conservation:**
   ```
   sum(outputs) == sum(inputs) + subsidy - fees
   (Total money never exceeds MAX_SUPPLY)
   ```

### Runtime Invariant Checks
```cpp
// Example: Money supply check
uint64_t total_out = 0;
for (const auto& tx : block.transactions) {
    for (const auto& out : tx.outputs) {
        total_out += out.value;
        assert(total_out <= MAX_SUPPLY);  // Consensus-critical
    }
}
```

---

## 6. How is reorg safety guaranteed?

### Undo Data Retention Policy (Explicit)

**Undo data MUST be retained if:**
1. Block is in active chain, OR
2. Block height ≥ (tip_height - MIN_BLOCKS_TO_KEEP)
   - MIN_BLOCKS_TO_KEEP = 288 blocks (~48 hours for 10-min blocks)
   - Ensures wallet rescan safety
   - Ensures reorg safety for reasonable depths

**Undo data MAY be deleted only if:**
1. Block depth > MIN_BLOCKS_TO_KEEP, AND
2. Block is not in active chain, AND
3. Pruning is enabled (if applicable)

**If undo data is missing during reorg:**
- **Action:** HARD STOP - Refuse to disconnect block
- **Log:** "ERROR: Cannot reorg, undo data missing for block {hash}"
- **Result:** Node stays on current chain
- **Recovery:** Require full reindex to rebuild undo data

**Critical Rule:**
- DisconnectBlock MUST NOT consume or delete undo data
- Undo data is reusable across multiple disconnect/reconnect cycles
- Undo data persists until explicitly pruned (separate maintenance task)

### Reorg Safety Principles

1. **Undo Data Completeness:**
   - Every connected block MUST have corresponding undo data
   - Undo data written BEFORE block marked as connected (Phase A before Phase B)
   - Undo data survives node restart (durable storage)
   - **Violation = Fatal Error:** Cannot reorg without undo

2. **Disconnect Order (Mandatory):**
   ```
   To reorg from height H to height H-N:
     for i = H down to (H-N+1):  // REVERSE order mandatory
       result = DisconnectBlock(block_at_height[i])
       if !result.success:
         FATAL ERROR → reorg aborted
   ```
   - Must disconnect in REVERSE order (newest to oldest)
   - Each disconnect uses its BlockUndo
   - Cannot skip blocks or disconnect out of order

3. **Atomic Disconnect (RocksDB Batch):**
   ```
   BEGIN RocksDB WriteBatch
     1. Load BlockUndo from disk (fail if missing)
     2. Validate undo data checksum
     3. For each tx in block (REVERSE order):
        a. Remove outputs from UTXO set
        b. Restore inputs from undo data
     4. Update block index:
        - Clear BLOCK_VALID_TRANSACTIONS flag
        - Keep BLOCK_VALID_SCRIPTS (if set)
     5. Update chainstate metadata:
        - best_block_hash = parent_hash
        - best_height = height - 1
   COMMIT RocksDB batch (atomic)
   ```

4. **DisconnectBlock Reusability (NOT Idempotence):**
   - ⚠️ **Correction:** DisconnectBlock is NOT idempotent
   - **Actual behavior:** Undo data is READ, not consumed
   - Calling `DisconnectBlock(X)` twice:
     - First call: Succeeds (block disconnected)
     - Second call: Fails (block already disconnected - precondition violation)
   - **Undo data persists** after disconnect (enables re-connect if needed)

### Reorg Attack Mitigation

1. **Minimum Chainwork Threshold:**
   - Refuse reorgs to chains below `nMinimumChainWork`
   - Updated every release (like assumevalid)
   - Prevents low-work eclipse attacks during IBD

2. **AssumeValid (IBD Speedup):**
   - Skip script checks below assumevalid height during IBD
   - Still validate PoW, structure, consensus rules
   - Still track chainwork for fork-choice

3. **Reorg Depth Logging (Observability):**
   - Log INFO for reorgs 2-6 blocks deep
   - Log WARNING for reorgs > 6 blocks deep
   - Log CRITICAL for reorgs > 100 blocks deep
   - **NO hard consensus limit** (trust minimum chainwork)

4. **UTXO Cache (Performance):**
   - Maintain in-memory cache of recent UTXOs
   - Speeds up repeated connects/disconnects during reorg
   - Flush to disk periodically (not after every block)

### Startup Recovery Sequence (Crash Safety)

**On node startup, execute in order:**

```
1. Load block index from disk
2. Load chainstate metadata (best_block_hash, best_height)

3. CONSISTENCY CHECKS:

   CHECK A: Does best_block exist in block index?
   - NO → FATAL: Corrupted chainstate → Require reindex
   - YES → Continue

   CHECK B: Does best_block have BLOCK_VALID_TRANSACTIONS?
   - NO → WARNING: Incomplete block connection → Roll back metadata
   - YES → Continue

   CHECK C: Does undo data exist for best_block?
   - NO → FATAL: Cannot reorg → Require reindex
   - YES → Continue

4. ORPHANED UNDO CLEANUP:

   For each undo file on disk:
     - Load corresponding block_hash
     - If block NOT in index:
       - Log: "Deleting orphaned undo for {hash}"
       - Delete undo file (safe - block was never connected)
     - If block in index but NOT connected:
       - Delete undo file (safe - block connection failed)

5. CHAINSTATE TIP ALIGNMENT:

   If block_index_tip > chainstate_tip:
     - Log: "Rolling forward chainstate metadata"
     - Update chainstate.best_block_hash
     - Update chainstate.best_height
     - (This handles crash between index update and metadata update)

6. Ready to accept new blocks
```

**Recovery Strategies:**

| Scenario | Detection | Recovery Action |
|----------|-----------|-----------------|
| Orphaned undo (Phase A ok, Phase B failed) | Undo exists, block not in index | Delete undo file |
| Missing undo (filesystem corruption) | Block in index, no undo | FATAL → Reindex |
| Incomplete connection (crash mid-commit) | Block in index, not validated | Roll back metadata |
| RocksDB corruption | Checksum failure on load | FATAL → Reindex |
| Metadata lag (crash after index update) | Index tip > chainstate tip | Roll forward metadata |

---

## 7. What G.3.4 Assumes Has Already Been Validated

**CRITICAL:** G.3.4 is ONLY responsible for state mutation. All consensus validation MUST be complete before ConnectBlock is called.

### Preconditions Enforced by Caller (G.3.1 → G.3.2 → G.3.3)

**G.3.2 (Structural Validation) guarantees:**
- Block size ≤ MAX_BLOCK_SIZE (4MB)
- Block header is well-formed (80 bytes)
- Merkle root matches transaction list
- Transactions are well-formed (deserializable)
- No duplicate transactions in block

**G.3.3 (Consensus Validation) guarantees:**
- All transaction inputs exist in UTXO set
- No duplicate inputs within transactions
- Coinbase shape is correct (null outpoint, 1 input)
- **Coinbase subsidy ≤ GetBlockSubsidy(height) + fees** ⚠️ **EXTENSION REQUIRED**
- Input values ≥ output values (fees implicit)
- No value overflow
- Scripts are valid (when script validation is implemented)
- Locktime/sequence rules enforced (when implemented)

### ⚠️ G.3.3 MUST Be Extended for Subsidy Validation

**Current Gap:**
- G.3.3 validates coinbase shape, but NOT subsidy amount
- This is a layer violation risk

**Required Change:**
```cpp
// In ConsensusValidator (G.3.3)
ConsensusValidationResult validateCoinbase(
    const Transaction& tx,
    uint32_t height,
    const ConsensusParams& params
) {
    // Existing: Shape validation
    if (tx.inputs.size() != 1) { ... }
    if (!tx.inputs[0].prevout.isNull()) { ... }

    // NEW: Subsidy validation
    uint64_t max_subsidy = GetBlockSubsidy(height, params);
    uint64_t coinbase_value = 0;
    for (const auto& out : tx.outputs) {
        coinbase_value += out.value;
    }

    // Note: fees are validated separately (inputs >= outputs for non-coinbase txs)
    // Coinbase can claim up to subsidy + fees, but fees are unknown here
    // Therefore: Only validate that coinbase doesn't EXCEED max possible
    // Full validation (subsidy + fees) happens during block validation when
    // all transaction fees are known.

    // For now: Just validate subsidy schedule is respected
    // Full check: coinbase_value <= subsidy + sum(fees) happens in G.3.4

    return Ok();
}
```

**Design Decision:**
- **Subsidy amount validation belongs in G.3.3** (consensus rule)
- **Fee accumulation belongs in G.3.4** (requires all transactions)
- **Combined check (subsidy + fees) happens in G.3.4** but assumes subsidy schedule is correct

### What G.3.4 Does NOT Re-Validate

G.3.4 **trusts** that G.3.3 has already verified:
- ✅ UTXO existence
- ✅ Duplicate input detection
- ✅ Value overflow protection
- ✅ Script validity (when implemented)
- ✅ Locktime/sequence (when implemented)

G.3.4 **only checks:**
- ❌ Money conservation (subsidy + fees = outputs for coinbase)
- ❌ Parent connection (already checked by caller)
- ❌ Height monotonicity (already checked by caller)

**Rationale:**
- Re-validation would be redundant and slow
- G.3.3 is pure and deterministic - if it passed once, it's valid
- G.3.4 focuses on state mutation, not re-checking truth

### Undo Data Size (Already Bounded)

**Question:** Can attacker create blocks with excessive undo data?

**Answer:** NO - already bounded by existing consensus rules

**Calculation:**
- MAX_BLOCK_SIZE = 4MB
- Max transactions per block ≈ 10,000 (realistic)
- Max inputs per transaction ≈ 5,000 (limited by block size)
- Max inputs per block ≈ 50,000 (worst case)
- Undo data per input ≈ 30-50 bytes (TxOut: value + scriptPubKey)
- **Max undo data ≈ 1.5-2.5 MB per block**

**Invariant (Documented):**
```
Undo data size is implicitly bounded by:
  - Block size limit (4MB)
  - Transaction size limit (100KB)
  - No separate undo size limit needed
```

**No additional validation required.**

---

## 8. Bitcoin Core Equivalence Mapping

### Direct Equivalents

| DineroCoin G.3.x | Bitcoin Core | Mutates State? | Validates Subsidy? | File |
|------------------|--------------|----------------|--------------------|------|
| G.3.1 (ValidationSink) | ProcessMessages() entry | ❌ | ❌ | net_processing.cpp |
| G.3.2 (StructuralValidator) | CheckBlock() (partial) | ❌ | ❌ | validation.cpp |
| G.3.3 (ConsensusValidator) | CheckBlock() + CheckTransaction() + CheckInputs() | ❌ | ✅ **Must add** | validation.cpp |
| **G.3.4 (ConnectBlock)** | **ConnectBlock()** | ✅ | ❌ (assumes done) | **validation.cpp** |
| **G.3.4 (DisconnectBlock)** | **DisconnectBlock()** | ✅ | ❌ | **validation.cpp** |
| G.3.5 (later) | ActivateBestChain() | ✅ | ❌ | validation.cpp |

**Note:** Bitcoin Core's `CheckBlock()` validates subsidy. DineroCoin must add this to G.3.3.

### Bitcoin Core ConnectBlock Signature
```cpp
// Bitcoin Core (validation.cpp)
bool ConnectBlock(
    const CBlock& block,
    CValidationState& state,
    CBlockIndex* pindex,
    CCoinsViewCache& view,
    const CChainParams& chainparams,
    bool fJustCheck = false
);
```

### Bitcoin Core DisconnectBlock Signature
```cpp
// Bitcoin Core (validation.cpp)
bool DisconnectBlock(
    const CBlock& block,
    CBlockIndex* pindex,
    CCoinsViewCache& view
);
```

### Key Differences to Note
1. **Bitcoin Core uses CCoinsViewCache** - We use IChainState interface
2. **Bitcoin Core has `fJustCheck` mode** - We separate into G.3.3 vs G.3.4
3. **Bitcoin Core inline undo generation** - We explicitly structure it
4. **Bitcoin Core tighter coupling** - We maintain cleaner layer separation
5. **Bitcoin Core validates subsidy in CheckBlock** - We must add to G.3.3

---

## 8. Implementation Plan (NOT STARTED)

### Step 1: Define Interfaces (3 days)
- `IChainState` interface (read-write UTXO access)
- `BlockUndo` serialization format
- `ConnectBlockResult` structure
- `ValidationFlags` enumeration

### Step 2: Implement ConnectBlock (5 days)
- Input validation (preconditions)
- Transaction application loop
- Undo data generation
- Invariant checking
- Error handling

### Step 3: Implement DisconnectBlock (3 days)
- Undo data loading
- Reverse transaction application
- UTXO restoration
- Invariant checking

### Step 4: Persistence Layer (2 days)
- Undo data serialization
- `rev*.dat` file management
- Atomic write semantics
- Flush and sync

### Step 5: Integration Tests (3 days)
- Test 1: Connect single block
- Test 2: Connect chain of blocks
- Test 3: Disconnect single block
- Test 4: Reorg (2-deep, 6-deep, 100-deep)
- Test 5: Crash recovery
- Test 6: Invalid block rejection
- Test 7: Money supply invariant
- Test 8: Undo data persistence

**Total Estimated Effort:** 16 days (3 weeks)

---

## 9. Testing Strategy

### Unit Tests (Pure Logic)
1. **BlockUndo Serialization:**
   - Round-trip serialization
   - Empty undo data
   - Large undo data (>1000 inputs)

2. **Invariant Checkers:**
   - Money supply check
   - Height monotonicity
   - Chainwork accumulation

### Integration Tests (State Mutation)
1. **ConnectBlock Success:**
   - Connect block to empty chain
   - Connect block to existing chain
   - UTXO set updated correctly
   - Undo data persisted

2. **ConnectBlock Failure:**
   - Invalid parent hash
   - UTXO missing (should never happen after G.3.3)
   - Disk full during undo write
   - Rollback verification

3. **DisconnectBlock:**
   - Disconnect tip
   - UTXO set restored correctly
   - Undo data consumed

4. **Reorg Scenarios:**
   - 2-block reorg (common)
   - 6-block reorg (rare)
   - 100-block reorg (stress test)
   - Competing chains with different work

5. **Crash Recovery:**
   - Crash during ConnectBlock
   - Crash after undo write, before index update
   - Restart and verify consistency

### Performance Tests
1. **Large Blocks:**
   - 4MB block with 10,000 transactions
   - Measure connect time
   - Target: < 1 second

2. **Deep Reorgs:**
   - 100-block reorg
   - Measure disconnect + reconnect time
   - Target: < 10 seconds

---

## 10. Risk Analysis

### High-Risk Areas

1. **Atomicity Violations:**
   - **Risk:** Partial block connections on crash
   - **Mitigation:** Database transactions, atomic file writes, fsync()

2. **Undo Data Corruption:**
   - **Risk:** Cannot reorg if undo data lost
   - **Mitigation:** Checksums, redundant storage, verification on load

3. **UTXO Set Corruption:**
   - **Risk:** Incorrect UTXO state breaks consensus
   - **Mitigation:** Invariant checks, paranoid mode (re-validate on load)

4. **Reorg Bugs:**
   - **Risk:** Money duplication, loss during reorg
   - **Mitigation:** Comprehensive reorg tests, money supply tracking

5. **Disk Full:**
   - **Risk:** Undo write fails, block connection fails
   - **Mitigation:** Pre-check disk space, graceful degradation

### Bitcoin Core Historical Bugs (Learn From)
- **CVE-2018-17144:** Duplicate inputs not checked properly → inflation bug
- **BDB lock limits:** Caused unintentional hard fork (0.7 vs 0.8)
- **Reorg money loss:** Early Bitcoin had reorg-related balance bugs

---

## 11. Success Criteria

G.3.4 is complete when ALL of the following are true:

### Functional
- ✅ ConnectBlock() applies transactions to chainstate
- ✅ DisconnectBlock() reverses transactions using undo data
- ✅ Undo data persists across node restart
- ✅ Reorgs work correctly (2-deep, 6-deep, 100-deep)
- ✅ Crash recovery restores consistent state

### Performance
- ✅ ConnectBlock() < 1s for 4MB block
- ✅ DisconnectBlock() < 100ms per block
- ✅ 100-block reorg < 10s total

### Safety
- ✅ All invariants enforced (money supply, height, chainwork)
- ✅ Atomic operations (no partial states)
- ✅ Crash-safe (no corruption on unexpected shutdown)

### Testing
- ✅ 8 integration tests passing
- ✅ Unit tests for undo serialization
- ✅ Reorg stress tests passing
- ✅ No memory leaks (valgrind clean)

---

## 12. What This Is NOT

G.3.4 does **NOT** implement:

- ❌ Fork-choice logic (ActivateBestChain - later phase)
- ❌ Mempool updates (mempool conflict removal - later phase)
- ❌ Network announcements (block relay - later phase)
- ❌ Mining integration (block template updates - later phase)
- ❌ Wallet updates (balance changes - later phase)

G.3.4 is ONLY responsible for:
- ✅ Applying validated block to chainstate
- ✅ Generating undo data
- ✅ Disconnecting block using undo data

---

## 13. Next Steps (After Authorization)

1. **Review this design document** - Confirm architectural soundness
2. **Define interfaces** - `IChainState`, `BlockUndo`, `ConnectBlockResult`
3. **Write tests FIRST** - Integration tests before implementation
4. **Implement ConnectBlock** - Core state mutation logic
5. **Implement DisconnectBlock** - Reorg support
6. **Freeze G.3.4** - Hard stop, no extensions

---

## Conclusion

G.3.4 is the **one-way door** from pure evaluation to state mutation.

This design document establishes:
- **Clear contracts** (inputs, outputs, invariants)
- **Reorg safety** (undo data, atomicity)
- **Bitcoin Core equivalence** (ConnectBlock, DisconnectBlock)
- **Risk mitigation** (atomicity, crash recovery)
- **Success criteria** (functional, performance, safety)

**This document must be approved before implementation begins.**

---

**Status:** Design stub awaiting review
**Implementation:** NOT STARTED
**Authorization Required:** YES
