# ChainDB Invariants - Protocol-Level Guarantees

**Status:** LOCKED (v0.10.0+)
**Authority:** Consensus-Critical
**Enforcement:** Compile-time + Runtime

---

## What This Document Defines

ChainDB invariants are **protocol-level guarantees**, not implementation details.

They define what can **never** change, regardless of:
- What features are added
- What refactors happen
- What contributors misunderstand
- What future-you forgets

**Violating these invariants = protocol bug = mainnet failure**

---

## The 5 NON-NEGOTIABLE Invariants

### 🔒 INVARIANT #1 — Single Writer Authority

**Rule:** ONLY BlockAcceptor may write to ChainDB.

**Enforcement:** ChainWriteToken (compile-time lock)

**Allowed:**
- `BlockAcceptor::ConnectBlock`
- `BlockAcceptor::DisconnectBlock`

**Forbidden:**
- Miner
- Network
- RPC
- Wallet
- Mempool
- Tests (unless explicitly opt-in)

**Why this matters:**
If any other component writes → phantom blocks, ASERT breaks, Utreexo desyncs, reorgs corrupt state.

This is the exact bug class Bitcoin spent a decade eliminating.

---

### 🔒 INVARIANT #2 — Atomicity

**Rule:** A block is either fully applied or not applied at all.

**What must be atomic:**
- UTXO set mutations
- TX index updates
- Height → hash index
- Utreexo accumulator state
- Undo data
- Tip advancement

**Implementation:** RocksDB WriteBatch (all-or-nothing commit)

**NEVER allowed:**
- Partial commits
- "Best effort" writes
- Catching exceptions and continuing

**Why this matters:**
If atomicity breaks → reorg rollback becomes impossible, indexes lie, wallets see ghosts.

---

### 🔒 INVARIANT #3 — Reorg Symmetry

**Rule:** `DisconnectBlock` must perfectly undo `ConnectBlock`.

For every operation in ConnectBlock, there must be a mirror in DisconnectBlock:

| ConnectBlock | DisconnectBlock |
|--------------|-----------------|
| `addUTXO` | `removeUTXO` |
| `spendUTXO` | `restoreUTXO` |
| `putTxIndex` | `deleteTxIndex` |
| `updateUtreexo(+leaf)` | `updateUtreexo(-leaf)` |
| `advance tip` | `rewind tip` |

**Real bug we fixed:** TX index rollback was missing (Phase 3 bug).
**Impact:** Reorg would leave phantom TX index entries → wallet confusion.

---

### 🔒 INVARIANT #4 — Read-Only Everywhere Else

**Rule:** All subsystems except BlockAcceptor are read-only consumers.

| Component | Allowed Operations |
|-----------|-------------------|
| Mempool | Read UTXOs (validation) |
| Network | Request data (sync) |
| RPC | Query state (getblock, etc.) |
| Wallet | Observe state (balance calc) |
| Miner | Read tip, mempool |

**NOT allowed:**
- Miner advancing height
- Network mutating UTXO set
- RPC with side effects
- Wallet modifying consensus data

**Why this matters:**
This prevents "SimpleBlockchain syndrome" (phantom state, race conditions, non-determinism).

---

### 🔒 INVARIANT #5 — Utreexo Consistency

**Rule:** UTXO mutations and Utreexo mutations are inseparable.

**Required:**
- No UTXO add/remove without Utreexo update
- No Utreexo update without UTXO add/remove

**Why this matters:**
Utreexo is core protocol (not optional). If UTXO/Utreexo desync → proofs fail → clients reject blocks.

---

## How Invariants Are LOCKED

### 1. Compile-Time Lock (ChainWriteToken)

```cpp
// Only BlockAcceptor can construct this token
class ChainWriteToken {
private:
    ChainWriteToken() = default;
    friend class BlockAcceptor;
public:
    // Non-copyable, non-movable
    ChainWriteToken(const ChainWriteToken&) = delete;
    ChainWriteToken& operator=(const ChainWriteToken&) = delete;
};
```

**Usage:**
```cpp
// BlockAcceptor (ALLOWED)
ChainWriteToken token; // ✅ compiles
chain_db->putCoin(token, txid, vout, coin);

// Miner (FORBIDDEN)
ChainWriteToken token; // ❌ compiler error: private constructor
chain_db->putCoin(token, txid, vout, coin); // ❌ never reached
```

**Result:** Physically impossible to write to ChainDB without BlockAcceptor.

---

### 2. Header Documentation (chain_db.h)

All 5 invariants are documented in the ChainDB header with this exact warning:

```
VIOLATIONS ARE PROTOCOL BUGS, NOT CODE BUGS
```

This prevents well-meaning "cleanup" PRs from destroying the chain.

---

### 3. Method Signatures

All write methods require `const ChainWriteToken&`:

```cpp
Status putCoin(const ChainWriteToken& token, ...);
Status deleteCoin(const ChainWriteToken& token, ...);
Status setTip(const ChainWriteToken& token, ...);
Status writeBatch(const ChainWriteToken& token, ...);
```

Read methods do NOT require a token:
```cpp
StatusOr<Coin> getCoin(...) const; // anyone can read
StatusOr<TipInfo> getTip() const;  // anyone can read
```

---

### 4. Atomic WriteBatch

BlockAcceptor MUST use WriteBatch for all mutations:

```cpp
rocksdb::WriteBatch batch;

// Add all mutations
chain_db->putCoin(token, txid, vout, coin, &batch);
chain_db->putTxIndex(token, txid, block_hash, offset, &batch);
chain_db->setTip(token, hash, height, work, &batch);

// Commit atomically (all-or-nothing)
chain_db->writeBatch(token, std::move(batch), /*sync=*/true);
```

If **any** mutation fails → **all** are rolled back.

---

## Mental Model

Think of it this way:

| Role | Analogy |
|------|---------|
| ChainDB | Ledger |
| BlockAcceptor | Judge |
| Everything else | Audience |

**The audience can observe.**
**Only the judge can change the verdict.**

---

## What Success Looks Like

After invariant locking:

```bash
# Try to write from Miner
ChainWriteToken token; // ❌ COMPILER ERROR

# Try to write from RPC
chain_db->putBlock(...); // ❌ COMPILER ERROR (no token)

# Try to write from Network
chain_db->setTip(...); // ❌ COMPILER ERROR (no token)

# BlockAcceptor writes
ChainWriteToken token; // ✅ compiles (friend class)
chain_db->putCoin(token, ...); // ✅ compiles
```

**Architecture is now provably correct at compile-time.**

---

## Testing Invariants

### Invariant Test Examples

```cpp
// Test: Miner cannot write to ChainDB
TEST(ChainDB_Invariants, MinerCannotWrite) {
    // This test should NOT compile if uncommented:
    // ChainWriteToken token; // ❌ private constructor
    // chain_db.putCoin(token, txid, vout, coin); // ❌
}

// Test: Partial batch must not corrupt state
TEST(ChainDB_Invariants, AtomicityRequired) {
    rocksdb::WriteBatch batch;
    // Add 100 UTXO mutations
    for (int i = 0; i < 100; i++) {
        chain_db.putCoin(token, txid_i, vout, coin, &batch);
    }

    // Simulate crash before commit
    // ... batch goes out of scope ...

    // Verify ZERO mutations applied
    for (int i = 0; i < 100; i++) {
        ASSERT_FALSE(chain_db.getCoin(txid_i, vout).ok());
    }
}

// Test: Reorg symmetry (DisconnectBlock undoes ConnectBlock)
TEST(ChainDB_Invariants, ReorgSymmetry) {
    // ConnectBlock
    BlockAcceptor::ConnectBlock(block_100);
    auto tip_after_connect = chain_db.getTip();

    // DisconnectBlock
    BlockAcceptor::DisconnectBlock(block_100);
    auto tip_after_disconnect = chain_db.getTip();

    // MUST return to exact same state
    ASSERT_EQ(tip_after_connect.height - 1, tip_after_disconnect.height);
    ASSERT_EQ(tip_before_connect.hash, tip_after_disconnect.hash);
}
```

---

## Version History

- **v0.10.0** - Invariants formally locked (December 13, 2025)
  - ChainWriteToken introduced
  - All write methods require token
  - Header documentation added
  - This document created

- **Pre-v0.10.0** - Invariants existed informally
  - Single-writer pattern enforced by discipline
  - Atomicity via WriteBatch (present since Phase 1)
  - Reorg symmetry (TX index bug fixed in Phase 3)

---

## If You Need to Modify ChainDB

**Ask yourself:**

1. Am I adding a read method? → No token needed, proceed.
2. Am I adding a write method? → Requires `const ChainWriteToken& token` parameter.
3. Am I calling a write method outside BlockAcceptor? → **STOP. Protocol violation.**

**If you think you need to weaken these guarantees:**

You don't. This has been proven by Bitcoin Core's 15-year evolution.

If you proceed anyway, you will create:
- Phantom balances (wallet bug)
- Reorg corruption (consensus bug)
- Utreexo desync (proof failure)
- Non-deterministic chain state (protocol death)

**Don't do it.**

---

## References

- `include/storage/chain_write_token.h` - Token implementation
- `include/storage/chain_db.h` - ChainDB header with invariant documentation
- `src/consensus/block_acceptor.cpp` - Only authorized writer
- Bitcoin Core `validation.cpp` - Equivalent single-writer authority

---

**This is not documentation.**
**This is protocol truth.**
