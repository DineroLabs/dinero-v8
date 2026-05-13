# UTXO Architecture - Bitcoin Core Model

**Date:** 2025-12-14
**Status:** Canonical Reference
**Verified:** 100-block deep reorg test passes

## Ground Truth

**DineroCoin follows the Bitcoin Core UTXO model: a single authoritative UTXO database (ChainDB coins CF) with layered in-memory views for policy and performance.**

## Current State (v0.10.0+)

```
ChainDB (RocksDB)
└── coins CF  ← Sole UTXO authority (Bitcoin Core's CoinsViewDB equivalent)
```

**Properties:**
- ✅ Single source of truth
- ✅ ChainWriteToken enforces single writer (BlockAcceptor only)
- ✅ WriteBatch atomicity (UTXOs commit with blocks/headers/undo)
- ✅ Reorg-safe (undo records stored in ChainDB)
- ✅ Proven correct via 100-block deep reorg test

**What writes to ChainDB coins CF:**
- BlockAcceptor::ConnectBlock (via putCoin/deleteCoin with token)
- BlockAcceptor::DisconnectBlock (undo via putCoin/deleteCoin with token)

**What reads from ChainDB coins CF:**
- BlockAcceptor::BuildUndoForBlock (getCoin)
- Wallet rescan (forEachUTXO)
- RPC queries (getCoin)

## Bitcoin Core Equivalence

| Bitcoin Core | DineroCoin | Status |
|--------------|------------|--------|
| CoinsViewDB (LevelDB) | ChainDB coins CF (RocksDB) | ✅ Implemented |
| CoinsViewCache (in-memory) | Not implemented | ⏳ Deferred (performance only) |
| CoinsViewMemPool (policy overlay) | Not implemented | 🎯 Next (v0.11.0 mempool hardening) |

## Future: Layered Views (NOT databases)

### Phase 1: CoinsViewMemPool (Policy Overlay) - NEXT

**Purpose:** Answer "what if all mempool txs were mined?"

**Implementation:**
```cpp
class MempoolUTXOView {
    std::set<Outpoint> spent_outpoints_;
    std::map<Outpoint, Coin> created_outputs_;
    ChainDB* base_db_;  // NOT owned, just a view

    StatusOr<Coin> getCoin(const Outpoint& out) {
        if (created_outputs_.count(out)) return created_outputs_[out];
        if (spent_outpoints_.count(out)) return Status::NotFound;
        return base_db_->getCoin(out.txid, out.vout);
    }
};
```

**Properties:**
- In-memory only (no RocksDB)
- No ChainWriteToken (never writes to ChainDB)
- No undo records (dropped on restart)
- Used for: double-spend checks, CPFP, RBF, package validation

**When to use:**
- Mempool transaction validation
- Fee estimation
- Ancestor/descendant checks

**When NOT to use:**
- Block validation (always use ChainDB directly)
- Wallet balance (wallets observe ChainDB only)

### Phase 2: CoinsViewCache (Performance Layer) - DEFERRED

**Only implement if profiler shows:**
- Block validation CPU-bound on UTXO lookups
- High TPS causing RocksDB bottleneck
- Deep reorg performance issues

**RocksDB already provides:**
- Block cache (configurable)
- Bloom filters
- Write batching
- Compression
- LRU eviction

**If added, must follow:**
- Write-through on ConnectBlock (cache mirrors ChainDB)
- Discardable on failure (never becomes authoritative)
- Atomic flush with WriteBatch
- ChainDB remains sole authority

## Anti-Patterns (NEVER DO)

❌ Multiple authoritative UTXO databases
❌ "Global" UTXO sets outside ChainDB
❌ Write tokens for policy layers
❌ Undo records for mempool views
❌ Caches that become authoritative
❌ Optimization without profiling

## Historical Note

**GlobalUTXOSet (2025-12-04 to 2025-12-14):**
- Created separate RocksDB for UTXOs
- Violated single authority principle
- Bypassed ChainWriteToken
- Not reorg-aware
- **Removed 2025-12-14** after deep reorg test verified ChainDB sufficiency

**Why it was wrong:**
- Created state bifurcation (ChainDB undo vs GlobalUTXOSet UTXOs)
- Two authoritative UTXO stores could desync
- Violated Bitcoin Core's single-database model

**Lesson learned:**
- Views are in-memory and never authoritative
- Databases are singular and token-protected
- Optimization requires measurement first

## Verification

**Deep Reorg Test (100 blocks):**
```bash
tests/reorg/test_deep_reorg.sh
```

**Success criteria:**
- ✅ ConnectBlock adds UTXOs via ChainDB::putCoin()
- ✅ DisconnectBlock restores UTXOs from undo records
- ✅ BuildUndoForBlock reads UTXOs via ChainDB::getCoin()
- ✅ WriteBatch commits atomically
- ✅ No corruption after 100-block reorg

**Last verified:** 2025-12-14 (PASSED)

## References

- Bitcoin Core `src/coins.h` (CoinsView hierarchy)
- Bitcoin Core `src/txmempool.h` (CTxMemPool UTXO tracking)
- DineroCoin `src/storage/chain_db.cpp` (ChainDB coins CF implementation)
- DineroCoin `docs/GLOBAL_UTXO_SET_REMOVAL_SANITY_CHECK.md` (removal verification)
