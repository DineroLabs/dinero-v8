# ActivateBestChain: FINAL FORM
**Never Revisit Edition**

**Philosophy:** Think in layers. Each layer locks permanently once complete.

---

## 🎯 CURRENT STATE MAPPING

| Layer | Item | DineroCoin Status | Gap |
|-------|------|-------------------|-----|
| **🟥 L1: Consensus** | | | |
| 1 | Fully Correct DisconnectBlock | ⚠️ 60% | Lines 241-256 TODO |
| 2 | Atomic Connect/Disconnect | ❌ 0% | No scoped write batch |
| 3 | Deterministic Fork Selection | ✅ 90% | Verify no peer influence |
| 4 | Undo Mandatory & Verified | ⚠️ 70% | No panic on missing |
| **🟧 L2: Safety** | | | |
| 5 | Rollback-on-Failure | ❌ 0% | Lines 147-150, 179-180 TODO |
| 6 | Real Block Loading | ❌ 0% | Lines 156-162 stubbed |
| 7 | BlockIndex = Source of Truth | ⚠️ 80% | Verify persistence |
| 8 | Idempotent Reorgs | ❌ 0% | Not tested |
| **🟨 L3: Mempool** | | | |
| 9 | Mempool Reconciliation | ✅ 80% | Exists, needs audit |
| 10 | Invalidation/Reconsideration | ❌ 0% | RPCs missing |
| 11 | Orphan Cleanup | ⚠️ 50% | Exists, needs hookup |
| **🟩 L4: Persistence** | | | |
| 12 | Restart Consistency | ❌ 0% | Not tested |
| 13 | Chainstate Self-Checks | ❌ 0% | No startup validation |
| 14 | Snapshot Compatibility | ❌ 0% | Not designed for |
| **🟦 L5: Hardening** | | | |
| 15 | Invariant Assertions | ❌ 10% | Minimal |
| 16 | Torture Test Suite | ❌ 0% | Doesn't exist |
| 17 | CI Reorg Gate | ❌ 0% | Doesn't exist |

**Overall Progress: ~30% (Framework exists, critical gaps remain)**

---

## 🟥 LAYER 1 — Consensus-Critical (NON-NEGOTIABLE)

### ✅ 1. Fully Correct DisconnectBlock()

**Current State:** ⚠️ **60% Complete**

**File:** `src/consensus/block_validation.cpp:232-259`

**What Works:**
- ✅ Restores spent inputs from undo data (line 245-253)
- ✅ Sanity checks undo height (line 234-237)

**What's Missing:**
```cpp
// Line 240-242: TODO
for (size_t i = block.vtx.size(); i > 1; --i) {
    // TODO: Parse transaction and remove its outputs from UTXO set
}

// Line 256: TODO
// TODO: Parse coinbase transaction and remove its outputs
```

**FINAL FORM Implementation:**
```cpp
bool BlockValidator::DisconnectBlock(const Block& block, uint32_t height,
                                    const BlockUndo& undo, std::string& error) {
    // Sanity check
    if (undo.height != height) {
        error = "Undo data height mismatch";
        return false;
    }

    // PHASE 1: Remove all non-coinbase transaction outputs (REVERSE ORDER)
    for (size_t i = block.vtx.size(); i > 1; --i) {
        const Transaction& tx = block.vtx[i - 1];
        std::string txid = TransactionParser::CalculateTxId(tx);

        // Remove all outputs created by this tx
        for (size_t n = 0; n < tx.vout.size(); n++) {
            if (!utxo_set_->SpendUTXO(txid, n, height)) {
                error = "Failed to remove tx output: " + txid + ":" + std::to_string(n);
                return false;
            }
        }
    }

    // PHASE 2: Restore all spent UTXOs (REVERSE ORDER)
    for (auto it = undo.spent_coins.rbegin(); it != undo.spent_coins.rend(); ++it) {
        const auto& entry = *it;

        // Restore the spent UTXO with EXACT state
        if (!utxo_set_->AddUTXO(entry.coin)) {
            error = "Failed to restore spent UTXO: " + entry.txid;
            return false;
        }
    }

    // PHASE 3: Remove coinbase outputs
    const Transaction& coinbase = block.vtx[0];
    std::string coinbase_txid = TransactionParser::CalculateTxId(coinbase);
    for (size_t n = 0; n < coinbase.vout.size(); n++) {
        if (!utxo_set_->SpendUTXO(coinbase_txid, n, height)) {
            error = "Failed to remove coinbase output: " + coinbase_txid + ":" + std::to_string(n);
            return false;
        }
    }

    // INVARIANT: UTXO set == exact state before this block was connected
    return true;
}
```

**Test Invariant:**
```cpp
// Before: UTXO set at height N
ConnectBlock(block_N+1);
// After: UTXO set at height N+1

DisconnectBlock(block_N+1);
// After: UTXO set MUST EXACTLY MATCH height N state

assert(utxo_hash_before == utxo_hash_after);
```

**🔒 Lock Criteria:**
- [ ] Removes all transaction outputs
- [ ] Removes all coinbase outputs
- [ ] Restores all spent inputs with exact flags
- [ ] Test: Connect → Disconnect → UTXO hash matches
- [ ] Test: Works across coinbase maturity boundary
- [ ] Never modify again

---

### ✅ 2. Atomic Connect/Disconnect Sequences

**Current State:** ❌ **0% - CRITICAL GAP**

**Problem:**
No scoped write batch. If reorg fails midway, UTXO is corrupted.

**FINAL FORM Implementation:**

```cpp
// Create atomic reorg guard
class ReorgGuard {
private:
    ChainDB* db_;
    UTXOSet* utxo_;
    rocksdb::WriteBatch* batch_;
    bool committed_ = false;

public:
    ReorgGuard(ChainDB* db, UTXOSet* utxo)
        : db_(db), utxo_(utxo) {
        // Start write batch (buffered in-memory)
        batch_ = new rocksdb::WriteBatch();
        utxo_->BeginBatch(batch_);
    }

    void Commit() {
        // Atomic write to disk
        rocksdb::WriteOptions opts;
        opts.sync = true;  // Force fsync
        db_->WriteBatch(opts, batch_);
        committed_ = true;
    }

    ~ReorgGuard() {
        if (!committed_) {
            // Reorg failed - batch is discarded
            // In-memory UTXO is untouched
            delete batch_;
            // No partial state on disk
        }
    }
};

// Usage in ActivateBestChain:
ReorgGuard guard(chain_db, utxo_set);

// Disconnect old blocks
for (auto* block : disconnect_path) {
    if (!DisconnectBlock(block, ...)) {
        // Guard destructor runs → batch discarded
        return ActivateBestChainResult::Fail("Disconnect failed");
    }
}

// Connect new blocks
for (auto* block : connect_path) {
    if (!ConnectBlock(block, ...)) {
        // Guard destructor runs → batch discarded
        return ActivateBestChainResult::Fail("Connect failed");
    }
}

// SUCCESS - commit atomically
guard.Commit();
```

**Guarantees:**
- Either ALL changes commit, or NONE do
- No partial UTXO state on disk
- Crash during reorg = full rollback on restart

**🔒 Lock Criteria:**
- [ ] RocksDB write batch implemented
- [ ] Scoped guard class with RAII
- [ ] Test: Kill -9 during reorg → restart → state consistent
- [ ] Test: ConnectBlock fails → UTXO unchanged
- [ ] Test: DisconnectBlock fails → UTXO unchanged
- [ ] Never modify again

---

### ✅ 3. Deterministic Fork Selection

**Current State:** ✅ **90% (Verify no peer influence)**

**File:** `src/consensus/chain_manager.cpp:107`

**Current Code:**
```cpp
CBlockIndex* best_candidate = GetBestCandidate();
```

**Audit Needed:**
```bash
# Verify chainwork is the ONLY factor
grep -rn "GetBestCandidate" src/consensus/chain_manager.cpp

# Check for peer influence
grep -rn "peer\|timestamp\|received_time" src/consensus/chain_manager.cpp
```

**FINAL FORM Rules:**
1. **Highest chainwork wins** (ONLY factor)
2. **Tie-breaker:** Lowest block hash (deterministic)
3. **NO peer influence**
4. **NO timestamp bias**
5. **NO "first seen" logic**

**Implementation:**
```cpp
CBlockIndex* ChainManager::GetBestCandidate() {
    CBlockIndex* best = nullptr;

    for (auto* candidate : block_index_) {
        if (!candidate->IsValid()) continue;

        if (!best) {
            best = candidate;
            continue;
        }

        // ONLY factor: chainwork
        if (candidate->nChainWork > best->nChainWork) {
            best = candidate;
        } else if (candidate->nChainWork == best->nChainWork) {
            // Tie-breaker: lowest hash (deterministic)
            if (candidate->GetBlockHash() < best->GetBlockHash()) {
                best = candidate;
            }
        }
    }

    return best;
}
```

**🔒 Lock Criteria:**
- [ ] Chainwork is ONLY selection factor
- [ ] Tie-breaker is deterministic (hash comparison)
- [ ] No peer state influences selection
- [ ] Test: Same blocks → same selection on different nodes
- [ ] Test: Network partition → deterministic reconvergence
- [ ] **NEVER MODIFY AGAIN** (this is consensus)

---

### ✅ 4. Undo Data is Mandatory, Verified, and Trusted

**Current State:** ⚠️ **70% (No panic on missing)**

**File:** `src/consensus/activate_best_chain.cpp:146`

**Current Code:**
```cpp
if (!undo_storage.hasUndo(block->hash)) {
    // Rollback: Reconnect any blocks we already disconnected
    return ActivateBestChainResult::Fail("Undo data missing for block");
}
```

**FINAL FORM Implementation:**
```cpp
// MANDATORY: Undo must exist
if (!undo_storage.hasUndo(block->hash)) {
    // This is FATAL - cannot proceed
    g_logger.critical("FATAL: Undo data missing for connected block");
    g_logger.critical("  Block: " + block->hash.GetHex());
    g_logger.critical("  Height: " + std::to_string(block->height));
    g_logger.critical("  This indicates database corruption");
    g_logger.critical("  Node cannot safely continue");

    // Option 1: Panic (safe)
    std::terminate();

    // Option 2: Enter safe mode (graceful)
    EnterSafeMode("Undo data missing - database corrupted");
    return ActivateBestChainResult::Fail("FATAL: Undo missing");
}

// VERIFIED: Validate checksum before use
BlockUndo undo;
if (!undo_storage.readUndo(block->hash, ..., &undo)) {
    g_logger.critical("FATAL: Undo data corrupted (checksum mismatch)");
    std::terminate();
}
```

**Startup Validation:**
```cpp
void ChainState::ValidateStartupInvariants() {
    // Every connected block MUST have undo
    for (CBlockIndex* block = tip; block && block->prev; block = block->prev) {
        if (block->IsConnected() && !undo_storage_->hasUndo(block->hash)) {
            g_logger.critical("STARTUP FATAL: Connected block missing undo");
            g_logger.critical("  Block: " + block->hash.GetHex());
            std::terminate();
        }
    }
}
```

**🔒 Lock Criteria:**
- [ ] Missing undo = panic (not silent fail)
- [ ] Undo checksum validated before use
- [ ] Startup check: all connected blocks have undo
- [ ] Undo only deleted after MIN_BLOCKS_TO_KEEP
- [ ] Test: Delete undo file → startup panics
- [ ] Never modify again

---

## 🟧 LAYER 2 — Chainstate Safety (Production-Grade)

### ✅ 5. Rollback-on-Failure Logic

**Current State:** ❌ **0% - TODO markers**

**File:** `src/consensus/activate_best_chain.cpp:147-150, 179-180`

**FINAL FORM Implementation:**
```cpp
// STEP 4: Disconnect Old Blocks
std::vector<BlockIndex*> disconnected_blocks;
Hash256 old_tip = chainstate.active_tip;

for (auto it = disconnect_path.rbegin(); it != disconnect_path.rend(); ++it) {
    BlockIndex* block = *it;

    auto disconnect_result = DisconnectBlock(...);

    if (!disconnect_result.ok) {
        // ROLLBACK: Reconnect everything we disconnected
        g_logger.error("DisconnectBlock failed - rolling back");

        for (auto* reconnect_block : disconnected_blocks) {
            auto reconnect_result = ConnectBlock(reconnect_block, ...);
            if (!reconnect_result.ok) {
                // DOUBLE FAULT: Cannot reconnect
                g_logger.critical("FATAL: Rollback failed - cannot reconnect old chain");
                g_logger.critical("  Original failure: " + disconnect_result.error);
                g_logger.critical("  Rollback failure: " + reconnect_result.error);

                // Database is corrupted - must terminate
                std::terminate();
            }
        }

        // Rollback successful - old chain restored
        chainstate.active_tip = old_tip;
        return ActivateBestChainResult::Fail("Disconnect failed: " + disconnect_result.error);
    }

    disconnected_blocks.push_back(block);
}

// STEP 5: Connect New Blocks
std::vector<BlockIndex*> connected_blocks;

for (BlockIndex* block : connect_path) {
    auto connect_result = ConnectBlock(...);

    if (!connect_result.ok) {
        // ROLLBACK: Disconnect what we connected, reconnect old chain
        g_logger.error("ConnectBlock failed - rolling back");

        // Phase 1: Disconnect new blocks we just connected
        for (auto it = connected_blocks.rbegin(); it != connected_blocks.rend(); ++it) {
            DisconnectBlock(*it, ...);
        }

        // Phase 2: Reconnect old chain
        for (auto* reconnect_block : disconnected_blocks) {
            auto reconnect_result = ConnectBlock(reconnect_block, ...);
            if (!reconnect_result.ok) {
                // DOUBLE FAULT
                g_logger.critical("FATAL: Rollback failed");
                std::terminate();
            }
        }

        // Rollback successful
        chainstate.active_tip = old_tip;
        return ActivateBestChainResult::Fail("Connect failed: " + connect_result.error);
    }

    connected_blocks.push_back(block);
}
```

**🔒 Lock Criteria:**
- [ ] Disconnect failure → reconnect old chain
- [ ] Connect failure → disconnect new + reconnect old
- [ ] Double fault → std::terminate()
- [ ] Test: Inject failure → verify rollback
- [ ] Test: Inject double fault → verify panic
- [ ] Never modify again

---

### ✅ 6. Block Loading Is Real (No Stubs)

**Current State:** ❌ **0% - Lines 156-162 stubbed**

**File:** `src/consensus/activate_best_chain.cpp:156-202`

**Current STUB:**
```cpp
// STUB: Use BlockIndex hash to create a distinguishing block
p2p::Block block_to_disconnect;
p2p::Transaction coinbase;
coinbase.version = 1;
p2p::TxOut output;
output.value = block->hash.data[0];  // FAKE!
```

**FINAL FORM Implementation:**
```cpp
// Load REAL block from disk
auto block_opt = block_storage_->ReadBlockFromDisk(block->hash);
if (!block_opt.has_value()) {
    g_logger.critical("FATAL: Block data missing for connected block");
    g_logger.critical("  Block: " + block->hash.GetHex());
    g_logger.critical("  Height: " + std::to_string(block->height));
    std::terminate();
}

const Block& block_to_disconnect = block_opt.value();

// Validate block hash matches
uint256 actual_hash = block_to_disconnect.GetHash();
if (actual_hash != block->hash) {
    g_logger.critical("FATAL: Block hash mismatch (corruption)");
    g_logger.critical("  Expected: " + block->hash.GetHex());
    g_logger.critical("  Actual: " + actual_hash.GetHex());
    std::terminate();
}
```

**Check if BlockStorage has this:**
```bash
grep -rn "ReadBlockFromDisk\|LoadBlock" src/storage/block_storage.cpp
```

**🔒 Lock Criteria:**
- [ ] Load real blocks from disk
- [ ] Validate hash after load
- [ ] Missing block data = panic
- [ ] Hash mismatch = panic
- [ ] Test: Delete blk file → startup panics
- [ ] Never modify again

---

### ✅ 7. BlockIndex Is the Single Source of Truth

**Current State:** ⚠️ **80% (Verify persistence)**

**Verification Needed:**
```bash
# Check what BlockIndex persists
grep -rn "nStatus\|nChainWork\|nHeight" include/storage/block_index.h

# Check if it survives restart
grep -rn "LoadBlockIndex\|WriteBlockIndex" src/storage/
```

**FINAL FORM Requirements:**

**BlockIndex must persist:**
- Block hash
- Chainwork
- Height
- Status flags (VALID_HEADER, VALID_TREE, VALID_TRANSACTIONS, VALID_CHAIN, BLOCK_CONNECTED)
- Undo location (file_id, offset, length, checksum)
- Parent hash

**Invariants:**
```cpp
// After restart:
assert(block_index->nStatus == persisted_status);
assert(block_index->nChainWork == persisted_chainwork);
assert(block_index->pprev->hash == persisted_prev_hash);

// BLOCK_CONNECTED requires undo
if (block_index->IsConnected()) {
    assert(undo_storage->hasUndo(block_index->hash));
}
```

**🔒 Lock Criteria:**
- [ ] All fields persist to disk
- [ ] Restart loads exact state
- [ ] No recomputation on startup
- [ ] Test: Restart → block status unchanged
- [ ] Test: Crash → block status unchanged
- [ ] Never modify again

---

### ✅ 8. Idempotent Reorgs

**Current State:** ❌ **0% - Not tested**

**FINAL FORM Requirement:**

Running ActivateBestChain twice with same input must:
1. Produce same result
2. Not double-apply changes
3. Not corrupt UTXO

**Implementation:**
```cpp
// STEP 1: Check if already active (IDEMPOTENT)
if (candidate_tip.hash == chainstate.active_tip) {
    // Already at this tip - return success immediately
    return ActivateBestChainResult::Ok(0, 0, candidate_tip.hash, candidate_tip.hash);
}
```

**Test:**
```cpp
// Test: Double activation
auto result1 = ActivateBestChain(candidate_A, ...);
assert(result1.ok);

auto result2 = ActivateBestChain(candidate_A, ...);
assert(result2.ok);
assert(result2.blocks_disconnected == 0);
assert(result2.blocks_connected == 0);

// UTXO hash unchanged
assert(utxo_hash_after_1 == utxo_hash_after_2);
```

**🔒 Lock Criteria:**
- [ ] Calling twice = no-op second time
- [ ] No double-apply of mutations
- [ ] Test: Activate → Activate → UTXO hash unchanged
- [ ] Test: Crash during activate → restart → activate completes
- [ ] Never modify again

---

## 🟨 LAYER 3 — Mempool & Network Correctness

### ✅ 9. Mempool Reconciliation

**Current State:** ✅ **80% (Exists, needs audit)**

**File:** `src/consensus/chain_manager.cpp` (ReconcileMempoolAfterReorg)

**Audit:**
```bash
grep -A30 "ReconcileMempoolAfterReorg" src/consensus/chain_manager.cpp
```

**FINAL FORM Requirements:**
1. Evict conflicted txs from mempool
2. Re-add valid txs from disconnected blocks
3. Re-evaluate ancestor limits
4. Maintain fee order
5. NO "flush and rebuild" hacks

**Test:**
```cpp
// Block A contains: tx1, tx2
// Block B contains: tx1, tx3 (conflicts with tx2)

// Mempool before reorg: [tx4, tx5]
ActivateBestChain(block_B);  // Reorg A → B

// Mempool after reorg should have:
// - tx2 (back from disconnected block A)
// - tx4, tx5 (unchanged)
// - NOT tx1 (now in block B)
// - NOT tx3 (now in block B)
```

**🔒 Lock Criteria:**
- [ ] Evict conflicted txs
- [ ] Re-add disconnected txs (if valid)
- [ ] Maintain correct order
- [ ] Test: Reorg → mempool correct
- [ ] Never modify again

---

### ✅ 10. Invalidation & Reconsideration

**Current State:** ❌ **0% - RPCs missing**

**FINAL FORM Implementation:**
```cpp
// RPC: invalidateblock <hash>
bool ChainManager::InvalidateBlock(const uint256& hash) {
    CBlockIndex* block = GetBlockIndex(hash);
    if (!block) return false;

    // Mark block and all descendants as INVALID
    block->nStatus |= BLOCK_FAILED_VALID;
    for (auto* desc : GetDescendants(block)) {
        desc->nStatus |= BLOCK_FAILED_CHILD;
    }

    // If this block is in active chain, reorg away from it
    if (IsInActiveChain(block)) {
        return ActivateBestChain();  // Find new best chain
    }

    return true;
}

// RPC: reconsiderblock <hash>
bool ChainManager::ReconsiderBlock(const uint256& hash) {
    CBlockIndex* block = GetBlockIndex(hash);
    if (!block) return false;

    // Clear INVALID flags
    block->nStatus &= ~(BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD);
    for (auto* desc : GetDescendants(block)) {
        desc->nStatus &= ~BLOCK_FAILED_CHILD;
    }

    // Reconsider this chain
    return ActivateBestChain();
}
```

**🔒 Lock Criteria:**
- [ ] invalidateblock RPC works
- [ ] reconsiderblock RPC works
- [ ] Test: Invalidate → reorg away
- [ ] Test: Reconsider → reorg back
- [ ] Essential for testing and emergency recovery
- [ ] Never modify again

---

### ✅ 11. Orphan / In-flight Cleanup

**Current State:** ⚠️ **50% (Exists, needs hookup)**

**Check:**
```bash
grep -rn "OrphanManager\|ClearOrphans" src/consensus/
```

**FINAL FORM Requirement:**

After reorg:
- Orphans referencing old chain are dropped
- In-flight block requests are revalidated
- Peer state updated

**🔒 Lock Criteria:**
- [ ] Orphans cleaned after reorg
- [ ] In-flight requests revalidated
- [ ] Test: Reorg → orphan pool correct
- [ ] Never modify again

---

## 🟩 LAYER 4 — Persistence & Restart Guarantees

### ✅ 12. Restart Consistency

**Current State:** ❌ **0% - Not tested**

**FINAL FORM Requirement:**

After crash + restart:
- Tip is correct
- UTXO matches tip
- BlockIndex status matches UTXO
- **NO rescan required**

**If restart requires rescan → you're not done.**

**Test:**
```bash
# Start node
./dinero-daemon

# Mine 100 blocks
dinero-cli generate 100

# Get tip
TIP=$(dinero-cli getbestblockhash)

# Kill node (SIGKILL - no graceful shutdown)
kill -9 $(pidof dinero-daemon)

# Restart
./dinero-daemon

# Verify tip unchanged
TIP2=$(dinero-cli getbestblockhash)
assert(TIP == TIP2)

# Verify UTXO unchanged
UTXO1=$(dinero-cli gettxoutsetinfo)
UTXO2=$(dinero-cli gettxoutsetinfo)
assert(UTXO1 == UTXO2)
```

**🔒 Lock Criteria:**
- [ ] Restart preserves tip
- [ ] Restart preserves UTXO
- [ ] NO rescan needed
- [ ] Test: Kill -9 → restart → state correct
- [ ] Never modify again

---

### ✅ 13. Chainstate Self-Checks

**Current State:** ❌ **0% - No startup validation**

**FINAL FORM Implementation:**
```cpp
void ChainState::ValidateStartup() {
    g_logger.info("Validating chainstate consistency...");

    // Check 1: UTXO best block matches tip
    uint256 utxo_best_block = utxo_set_->GetBestBlock();
    if (utxo_best_block != active_tip_) {
        g_logger.critical("FATAL: UTXO best block mismatch");
        g_logger.critical("  UTXO:   " + utxo_best_block.GetHex());
        g_logger.critical("  Tip:    " + active_tip_.GetHex());
        std::terminate();
    }

    // Check 2: Undo exists for tip-1
    if (active_tip_ != genesis) {
        CBlockIndex* prev = GetBlockIndex(active_tip_)->pprev;
        if (prev && !undo_storage_->hasUndo(active_tip_)) {
            g_logger.critical("FATAL: Undo missing for tip");
            std::terminate();
        }
    }

    // Check 3: BlockIndex consistency
    for (auto* block : block_index_) {
        if (block->IsConnected()) {
            // Connected blocks MUST have undo
            if (block != genesis && !undo_storage_->hasUndo(block->hash)) {
                g_logger.critical("FATAL: Connected block missing undo");
                std::terminate();
            }
        }
    }

    g_logger.info("Chainstate validation: PASSED");
}
```

**🔒 Lock Criteria:**
- [ ] Startup validation checks UTXO/tip match
- [ ] Startup validation checks undo exists
- [ ] Startup validation checks BlockIndex consistency
- [ ] Test: Corrupt UTXO → startup panics
- [ ] Fail fast on inconsistency
- [ ] Never modify again

---

### ✅ 14. Snapshot / AssumeUTXO Compatibility

**Current State:** ❌ **0% - Not designed for**

**FINAL FORM Requirement (Future-Proofing):**

Even if you don't enable snapshots now:
1. UTXO root must be serializable
2. BlockIndex must support skip-to-height
3. Undo must be optional past snapshot

**Minimal Design:**
```cpp
struct UTXOSnapshot {
    uint256 block_hash;
    uint32_t height;
    uint256 utxo_root;  // Merkle root or hash
    uint64_t total_coins;
};

// UTXO set must support:
uint256 GetUTXORoot() const;

// BlockIndex must support:
bool LoadFromSnapshot(const UTXOSnapshot& snapshot);
```

**🔒 Lock Criteria:**
- [ ] UTXO root is computable
- [ ] BlockIndex can skip to height
- [ ] Design allows future snapshot feature
- [ ] No implementation needed now (just interfaces)
- [ ] Never modify interfaces once locked

---

## 🟦 LAYER 5 — "Never Touch Again" Hardening

### ✅ 15. Invariant Assertions (Compile-Time Mental Locks)

**Current State:** ❌ **10% - Minimal**

**FINAL FORM Implementation:**
```cpp
// In ConnectBlock:
bool ConnectBlock(...) {
    // ... mutations ...

    // INVARIANT: UTXO best block must match new tip
    assert(utxo_view.GetBestBlock() == new_tip_hash);

    // INVARIANT: All inputs spent must have undo entries
    assert(undo.spent_coins.size() == total_inputs_spent);

    return true;
}

// In DisconnectBlock:
bool DisconnectBlock(...) {
    // ... mutations ...

    // INVARIANT: UTXO best block rolled back
    assert(utxo_view.GetBestBlock() == prev_block_hash);

    return true;
}

// In ActivateBestChain:
ActivateBestChainResult ActivateBestChain(...) {
    uint256 utxo_hash_before = utxo_view.GetHash();

    // ... reorg ...

    // INVARIANT: Idempotent
    if (blocks_disconnected == 0 && blocks_connected == 0) {
        assert(utxo_view.GetHash() == utxo_hash_before);
    }

    return result;
}
```

**🔒 Lock Criteria:**
- [ ] Assertions in all critical paths
- [ ] Assertions check UTXO consistency
- [ ] Assertions check undo correctness
- [ ] Release builds keep assertions (don't #ifdef out)
- [ ] Never remove assertions

---

### ✅ 16. Torture Test Suite (One-Time)

**Current State:** ❌ **0% - Doesn't exist**

**FINAL FORM Test Suite:**

```cpp
// tests/torture/test_activate_best_chain.cpp

TEST(ActivateBestChain, DeepReorg100Blocks) {
    // Mine chain A (100 blocks)
    // Mine chain B (101 blocks, higher chainwork)
    // Activate B → verify 100 block reorg works
    // Verify UTXO correctness
}

TEST(ActivateBestChain, ReorgAcrossCoinbaseMaturity) {
    // Mine 100 blocks with coinbase
    // Spend coinbase in block 101
    // Mine competing chain without that spend
    // Reorg → verify coinbase spend is reverted
}

TEST(ActivateBestChain, ReorgWithMempoolPressure) {
    // Fill mempool with 10,000 txs
    // Reorg 50 blocks
    // Verify mempool reconciliation correct
}

TEST(ActivateBestChain, CrashDuringReorg) {
    // Start reorg
    // Kill -9 after DisconnectBlock #5
    // Restart
    // Verify state consistent (old chain or new chain, not corrupted)
}

TEST(ActivateBestChain, RestartDuringReorg) {
    // Start reorg
    // Graceful shutdown after DisconnectBlock #5
    // Restart
    // Activate again
    // Verify reorg completes correctly
}

TEST(ActivateBestChain, IdempotentReorg) {
    // Activate chain A
    // Activate chain A again
    // Verify no mutations second time
}

TEST(ActivateBestChain, RollbackOnDisconnectFailure) {
    // Inject failure in DisconnectBlock #5
    // Verify rollback reconnects blocks 1-4
    // Verify old chain restored
}

TEST(ActivateBestChain, RollbackOnConnectFailure) {
    // Disconnect 10 blocks
    // Inject failure in ConnectBlock #5
    // Verify rollback: disconnect new 1-4, reconnect old 1-10
}

TEST(ActivateBestChain, MissingUndoPanics) {
    // Delete undo file for block 50
    // Try to disconnect block 50
    // Verify: std::terminate() called
}

TEST(ActivateBestChain, MissingBlockDataPanics) {
    // Delete blk file for block 50
    // Try to reorg through block 50
    // Verify: std::terminate() called
}
```

**Write once. Run forever in CI.**

**🔒 Lock Criteria:**
- [ ] All 10 tests pass
- [ ] Tests run in CI on every commit
- [ ] Tests never disabled
- [ ] If test fails → fix code, not test
- [ ] Never modify tests once they pass

---

### ✅ 17. CI Reorg Gate

**Current State:** ❌ **0% - Doesn't exist**

**FINAL FORM CI Configuration:**

```yaml
# .github/workflows/reorg_gate.yml
name: ActivateBestChain Protection

on: [push, pull_request]

jobs:
  reorg-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Build
        run: |
          mkdir build && cd build
          cmake .. && make

      - name: Run Reorg Torture Tests
        run: |
          cd build
          ./tests/torture/test_activate_best_chain

      - name: Verify No Regressions
        run: |
          # Check for chainstate corruption
          if grep -r "UTXO mismatch" test_output.log; then
            echo "FAIL: Chainstate corruption detected"
            exit 1
          fi

          # Check for undo violations
          if grep -r "Undo missing" test_output.log; then
            echo "FAIL: Undo data violation"
            exit 1
          fi

          echo "PASS: All reorg tests clean"
```

**This prevents future self-sabotage.**

**🔒 Lock Criteria:**
- [ ] CI runs reorg tests on every PR
- [ ] CI fails if any test fails
- [ ] CI cannot be bypassed
- [ ] Tests must pass before merge
- [ ] Never disable gate

---

## 🏁 IMPLEMENTATION ROADMAP

### **Phase 1: Consensus-Critical (L1) — MUST BE DONE**

**Estimated: 15-20 hours**

1. Complete DisconnectBlock (3h)
2. Implement Atomic Write Batch (4h)
3. Verify Fork Selection (2h)
4. Add Undo Validation + Panic (3h)
5. Test L1 invariants (3-5h)

**Deliverable:** Reorgs work correctly and safely

---

### **Phase 2: Safety (L2) — PRODUCTION-GRADE**

**Estimated: 12-16 hours**

5. Implement Rollback Logic (6h)
6. Hook Up Real Block Loading (4h)
7. Verify BlockIndex Persistence (2h)
8. Test Idempotent Reorgs (2h)
9. Test L2 invariants (2-4h)

**Deliverable:** Reorgs survive failures

---

### **Phase 3: Mempool & Persistence (L3 + L4) — CORRECTNESS**

**Estimated: 10-14 hours**

9. Audit Mempool Reconciliation (3h)
10. Add Invalidation RPCs (3h)
11. Hook Up Orphan Cleanup (2h)
12. Test Restart Consistency (4h)
13. Add Startup Validation (2h)
14. Design Snapshot Compatibility (2h)

**Deliverable:** Restart works, mempool correct

---

### **Phase 4: Hardening (L5) — LOCK FOREVER**

**Estimated: 8-12 hours**

15. Add Invariant Assertions (2h)
16. Write Torture Test Suite (6h)
17. Set Up CI Reorg Gate (2h)

**Deliverable:** **NEVER TOUCH AGAIN**

---

**Total Estimated Effort:** 45-62 hours (1-1.5 weeks full-time)

---

## 🎯 SUCCESS CRITERIA

### **L1 Locked:**
- [ ] DisconnectBlock works perfectly
- [ ] Atomic commits (no partial state)
- [ ] Fork selection is deterministic
- [ ] Undo is mandatory + verified

### **L2 Locked:**
- [ ] Rollback works on all failures
- [ ] Block loading is real (no stubs)
- [ ] BlockIndex persists correctly
- [ ] Reorgs are idempotent

### **L3 Locked:**
- [ ] Mempool reconciles correctly
- [ ] Invalidation/reconsideration works
- [ ] Orphan cleanup works

### **L4 Locked:**
- [ ] Restart preserves state (no rescan)
- [ ] Startup validation catches corruption
- [ ] Snapshot-compatible design

### **L5 Locked:**
- [ ] Assertions prevent regressions
- [ ] Torture tests pass forever
- [ ] CI blocks bad PRs

---

## 🔒 THE FINAL LOCK

Once all 17 items are complete:

**ActivateBestChain is DONE FOREVER.**

No more changes.
No more refactoring.
No more "improvements."

**It becomes consensus infrastructure that NEVER CHANGES.**

Like Bitcoin Core's ActivateBestChain (unchanged since 2015).

---

**This is the FINAL FORM.**
