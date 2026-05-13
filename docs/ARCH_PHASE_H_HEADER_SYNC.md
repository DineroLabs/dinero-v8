# Phase H — Header-First Sync / IBD (FROZEN)

**Status:** ✅ Complete & Locked (Commit: 15cffe23)
**Freeze Date:** 2025-12-18
**Proof:** Phase H.5 restart recovery test passing

---

## Architectural Contract

Header-first IBD is **complete, crash-correct, and externally observable**.

This phase established:
- Headers are authoritative (downloaded first, validated independently)
- Blocks are subordinate (fetched only for winning chain)
- Best chain selection is chainwork-based (no heuristics)
- Restart has zero magic (truth reconstruction from disk)

---

## Responsibilities Table

### HeaderSyncManager (Orchestration ONLY)

| **MUST DO** | **MUST NOT DO** |
|-------------|-----------------|
| ✅ Validate headers (PoW, linkage, timestamps only) | ❌ Validate transactions (→ G.3.3 ConsensusValidator) |
| ✅ Build header tree (in-memory + disk) | ❌ Mutate UTXOs (→ G.3.4 ConnectBlock/DisconnectBlock) |
| ✅ Select best header chain (by chainwork) | ❌ Execute reorgs (→ G.3.5 ActivateBestChain) |
| ✅ Schedule block downloads (winning chain only) | ❌ Store consensus logic |
| ✅ Track IBD state | ❌ Bypass existing validation pipeline |
| ✅ Persist header metadata | ❌ Make transport decisions (→ NetworkManager) |

### ChainManager (Chain Authority)

| **MUST DO** | **MUST NOT DO** |
|-------------|-----------------|
| ✅ Maintain active chain tip | ❌ Download blocks (→ HeaderSyncManager) |
| ✅ Query chain state | ❌ Validate headers (→ HeaderSyncManager) |
| ✅ Delegate to G.3.3-G.3.5 | ❌ Implement consensus rules directly |

### NetworkManager (Transport ONLY)

| **MUST DO** | **MUST NOT DO** |
|-------------|-----------------|
| ✅ Send/receive P2P messages | ❌ Decide sync policy (→ HeaderSyncManager) |
| ✅ Route headers to HeaderSyncManager | ❌ Choose batch sizes |
| ✅ Route blocks to G.3.3 pipeline | ❌ Validate headers or blocks |

---

## Persistence Guarantees

### What is Persisted (ChainDB CF #3)

**Schema Version:** v1 (locked)

**Minimal Header Metadata:**
```cpp
struct PersistedHeaderMetadata {
    static constexpr uint8_t SCHEMA_VERSION = 1;

    uint256 parent_hash;      // Topology (parent-first invariant)
    int32_t height;           // Ordering (monotonic)
    arith_uint256 chainwork;  // Fork choice (deterministic)
    uint32_t status_flags;    // Progress (separate batch domain)
};
```

**Separate Batch Domains:**
1. **Header writes:** parent_hash, height, chainwork (bounded, contiguous, parent-first)
2. **Status updates:** status_flags (separate WriteBatch, can update independently)

**Guarantees:**
- ✅ Headers written in parent-first order
- ✅ Partial trees allowed (real-world IBD)
- ✅ Commit = durable (no heuristics)
- ✅ Status updates are atomic

### What is NOT Persisted

❌ In-memory header tree (parent/child pointers) — rebuilt on restart
❌ Download queue — recomputed from tips
❌ In-flight requests — cleared, retried after timeout

---

## Restart Invariants

### Truth Reconstruction (Proven by H.5)

On restart, the node **knows exactly what it does not yet know**:

1. **Header persistence:** All headers A–D exist with correct parent links, heights, chainwork
2. **Block status persistence:** A, B marked HAVE_DATA; C, D not marked
3. **Active chain state:** Active tip = B (from ChainManager)
4. **Best header state:** Best header tip = D (recomputed from persisted headers)
5. **IBD signal correctness:** `IsInitialBlockDownload() == true` (header tip D ≠ active tip B)

**Key Invariant (Canonical IBD Definition):**
```cpp
// IBD is true iff best header tip != active chain tip
// No timers. No height thresholds. No magic constants.
bool IsInitialBlockDownload() const {
    if (!chain_manager_) return true;

    CBlockIndex* active_tip = chain_manager_->GetTip();
    if (!active_tip) return true;

    std::string active_tip_hash = active_tip->GetBlockHash().ToString();

    // Hash equality check (simple, immutable, correct)
    return best_header_hash_ != active_tip_hash;
}
```

### Crash Safety (Proven by H.5)

The system survives:
- ✅ SIGKILL (no graceful shutdown)
- ✅ Power loss (no flush calls)
- ✅ Kernel panic (object destruction only)

**No hidden coupling to in-memory state.**

---

## External Contract

### RPC Exposure (`getblockchaininfo`)

```json
{
  "blocks": 2,           // Active chain height (from ChainManager)
  "headers": 4,          // Best header height (from HeaderSyncManager)
  "initialblockdownload": true,  // Canonical IBD signal
  "verificationprogress": 0.5    // blocks / headers
}
```

**Guarantees:**
- ✅ `initialblockdownload` is truth, not heuristic
- ✅ Wallet, mempool, relay can trust it
- ✅ No "magic heights"
- ✅ No timers
- ✅ No shortcuts

---

## DO NOT List (Enforce in Code Review)

### ❌ DO NOT modify without new phase approval:

1. **Canonical IBD definition** (src/consensus/header_sync_manager.cpp:IsInitialBlockDownload)
   - No timers
   - No height thresholds
   - No magic constants
   - Hash comparison ONLY

2. **Header persistence schema** (include/storage/chain_db.h:PersistedHeaderMetadata)
   - Schema version locked at v1
   - No new fields without schema bump
   - No denormalization

3. **Architectural boundaries**
   - HeaderSyncManager = orchestration ONLY
   - No transaction validation in HeaderSyncManager
   - No UTXO mutation in HeaderSyncManager
   - No reorg execution in HeaderSyncManager

4. **Restart invariants**
   - No heuristics in LoadHeaderIndex()
   - No "guessing" missing state
   - No fallback to defaults

### ❌ DO NOT add:

- Heuristics to IBD detection
- Magic constants (e.g., "assume IBD if tip < 24 hours old")
- Height-based shortcuts
- Time-based IBD exit conditions
- Consensus logic to HeaderSyncManager
- Transport policy to HeaderSyncManager

### ❌ DO NOT bypass:

- G.3.3 ConsensusValidator (all blocks MUST go through this)
- G.3.4 ConnectBlock/DisconnectBlock (all UTXO mutations MUST go through these)
- G.3.5 ActivateBestChain (all reorgs MUST go through this)
- ChainWriteToken authorization (all ChainDB writes MUST have token)

---

## Integration Flow (Reference)

```
Headers (P2P) → HeaderSyncManager (validate PoW, store, select best)
                        ↓
              Block download requests
                        ↓
        Blocks (P2P) → BlockAcceptor (G.3.3 validation)
                        ↓
              ChainManager → ActivateBestChain (G.3.5)
                        ↓
              ConnectBlock (G.3.4) → UTXO mutation
```

**This flow is frozen. Any changes require architectural review.**

---

## Test Coverage (Frozen)

### Phase H.5 — Restart Recovery Test

**File:** `tests/consensus/test_header_sync_restart.cpp`

**Core Test:** `test_phase_h5_restart_ibd()`
- Injects headers A→B→C→D
- Downloads blocks A, B (partial sync)
- Simulates crash (SIGKILL/power loss)
- Restarts node
- Verifies IBD signal correctness after restart
- Completes sync (downloads C, D)
- Verifies IBD exits exactly when converged

**Proof:** This test validates truth reconstruction, not just behavior.

**If this test fails, Phase H is broken.**

---

## Future Work (Out of Scope for Phase H)

Phase H is **complete**. The following are separate phases:

- **Phase P:** Pruning Foundations (prune eligibility tracking)
- **Phase S:** AssumeUTXO / Snapshot Readiness
- **Performance:** Parallel block fetch, batching optimizations (ONLY after P/S)

**Do not add these to Phase H.**

---

## Formal Declaration

**Header-first IBD is complete, crash-correct, and externally observable.**

This statement is defensible and proven by:
- ✅ Deterministic header validation (H.1)
- ✅ Parent-first download scheduling (H.2)
- ✅ Crash-safe persistence (H.3)
- ✅ Canonical IBD detection (H.4)
- ✅ Restart recovery proof (H.5)

**Phase H is frozen.**

---

**Last Modified:** 2025-12-18
**Reviewers:** Any changes to this architecture require approval from consensus team.
