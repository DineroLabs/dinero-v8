# Database Refactor - Complete Status Report

**Date:** December 4, 2025
**Objective:** Bitcoin Core-compatible database architecture

---

## ✅ **COMPLETED WORK**

### **1. Created GlobalUTXOSet (RocksDB)**
- **File:** `include/consensus/global_utxo_set.h` + `src/consensus/global_utxo_set.cpp`
- **Purpose:** Consensus-critical global UTXO set (ALL blockchain UTXOs)
- **Storage:** `~/.dinero/blockchain/utxo/` (RocksDB directory)
- **Features:**
  - Atomic batch operations for block processing
  - Binary serialization for compact storage
  - LZ4 compression, bloom filters
  - Verification and statistics

### **2. Created WalletUTXOTracker (SQLite)**
- **File:** `include/wallet/wallet_utxo_tracker.h` + `src/wallet/wallet_utxo_tracker.cpp`
- **Purpose:** Per-wallet UTXO tracking (which UTXOs belong to THIS wallet)
- **Storage:** `~/.dinero/wallets/{name}/wallet.db` (creates `wallet_utxos` table)
- **Features:**
  - Derivation paths (BIP32)
  - Coin control (lock/unlock)
  - User labels
  - Validates against GlobalUTXOSet

### **3. Removed Wrong SQLite Databases**
- **File:** `src/database/sqlite_manager.cpp` + `.h`
- **Removed:**
  - `getBlockchainDB()` - explorer.db should not exist
  - `getMempoolDB()` - mempool should be RAM-only
  - `getPeersDB()` - peers should be binary
- **Status:** ✅ Code cleaned, methods removed

### **4. Documentation**
- **Created:**
  - `ARCHITECTURE_DB.md` - Complete database architecture
  - `REFACTOR_TODO.md` - Tracking remaining work
  - `SCHEMA_AUDIT.md` - Schema conflict analysis
  - `MEMPOOL_ARCHITECTURE.md` - Mempool RAM-only explanation
  - `SCHEMA_CONSISTENCY_CHECK.md` - UTXO table migration plan
  - `DATABASE_REFACTOR_STATUS.md` (this file)

---

## 🚨 **CRITICAL FINDINGS**

### **Finding 1: Conflicting Schema Files**

**Problem:** Schema files exist for databases we removed

| File | Location | Status | Action |
|------|----------|--------|--------|
| `explorer_schema.sql` | `database/schema/` | ❌ DELETE | Conflicts with GlobalUTXOSet |
| `mempool_schema.sql` | `database/schema/` | ❌ DELETE | Mempool must be RAM-only |
| `peers_schema.sql` | `database/schema/` | ❌ DELETE | Peers should be binary |
| `wallet_schema.sql` | `database/schema/` | ❌ DELETE | Old version (120 lines) |
| `wallet_schema.sql` | `resources/schema/` | ✅ KEEP | Correct version (371 lines with Lightning) |

**Impact:** Code references wrong schemas in `sqlite_manager.cpp`

---

### **Finding 2: Two UTXO Tables in Wallet**

**Problem:** wallet.db has two UTXO tables with different schemas

**Table 1: `utxos`** (Legacy)
- Defined in `wallet_schema.sql`
- Has: address, script_pubkey, is_coinbase, spent_txid, confirmations
- Missing: derivation_path, is_locked, label

**Table 2: `wallet_utxos`** (New)
- Created by `WalletUTXOTracker::initialize()`
- Has: derivation_path, cached_amount, cached_height, is_locked, label
- Missing: address, script_pubkey (gets from GlobalUTXOSet)

**Risk:** Data duplication and inconsistency

**Solution:** Add `wallet_utxos` to wallet_schema.sql, migrate from `utxos`, eventually remove `utxos`

---

### **Finding 3: Mempool Architecture Misunderstanding**

**Original Mistake:** Thought mempool should use RocksDB

**Correct Understanding (Bitcoin Core model):**
- ✅ Active mempool is **RAM-only** (`std::unordered_map`)
- ✅ Optional `mempool.dat` binary dump on shutdown (convenience)
- ❌ NO SQLite, NO RocksDB, NO LevelDB for active mempool

**Why RAM-Only:**
1. Thousands of changes per second
2. Non-consensus, volatile data
3. Disk I/O would create bottlenecks

---

### **Finding 4: Lightning Tables (No Issues)**

**Status:** ✅ Correctly integrated

Lightning tables in `wallet_schema.sql`:
- `ln_channels` - Channel state
- `ln_htlcs` - Hash Time-Locked Contracts
- `ln_commitments` - Commitment transactions
- `ln_peers` - Lightning peer connections
- `ln_invoices` - BOLT #11 invoices
- `ln_payments` - Payment history
- `ln_secrets` - Revocation secrets (encrypted)

**Why This Is Correct:**
- Lightning state is wallet-specific (not consensus)
- Each wallet has its own Lightning node identity
- SQLite is appropriate for wallet-level data

---

## 📋 **REMAINING WORK**

### **Phase 1: Fix Schema Files** (HIGH PRIORITY)

**Delete wrong schemas:**
```bash
rm database/schema/explorer_schema.sql
rm database/schema/mempool_schema.sql
rm database/schema/peers_schema.sql
rm database/schema/wallet_schema.sql
rmdir database/schema/  # Remove entire directory
```

**Update sqlite_manager.cpp:**
- Remove lines 614-620 (explorer schema loading)
- Remove lines 748-754 (mempool schema loading)
- Remove lines 822-828 (peers schema loading)
- Remove lines 491-498 (old wallet schema loading)

---

### **Phase 2: Fix wallet_utxos Schema** (HIGH PRIORITY)

**Add to `resources/schema/wallet_schema.sql`:**

```sql
-- ═══════════════════════════════════════════════════════════════════════════
-- Wallet UTXO Tracker (NEW - Bitcoin Core-compatible)
-- ═══════════════════════════════════════════════════════════════════════════
-- Tracks which UTXOs from GlobalUTXOSet belong to this wallet
-- Replaces legacy 'utxos' table with cleaner separation of concerns

CREATE TABLE IF NOT EXISTS wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,          -- BIP32 path (e.g., "m/84'/1447'/0'/0/5")
    cached_amount INTEGER NOT NULL,         -- Cached from GlobalUTXOSet
    cached_height INTEGER NOT NULL,         -- Cached from GlobalUTXOSet
    is_spent INTEGER NOT NULL DEFAULT 0,    -- Spent by this wallet
    is_locked INTEGER NOT NULL DEFAULT 0,   -- Locked for coin control
    label TEXT,                             -- User-defined label
    PRIMARY KEY (txid, vout)
);

CREATE INDEX IF NOT EXISTS idx_wallet_utxos_unspent ON wallet_utxos(is_spent) WHERE is_spent = 0;
CREATE INDEX IF NOT EXISTS idx_wallet_utxos_path ON wallet_utxos(derivation_path);
CREATE INDEX IF NOT EXISTS idx_wallet_utxos_height ON wallet_utxos(cached_height);
```

**Mark legacy table:**
```sql
-- ═══════════════════════════════════════════════════════════════════════════
-- Legacy UTXO Table (DEPRECATED)
-- ═══════════════════════════════════════════════════════════════════════════
-- Will be removed in future release
-- Use 'wallet_utxos' instead

CREATE TABLE IF NOT EXISTS utxos (
    ...existing schema...
);
```

---

### **Phase 3: Update Code References** (MEDIUM PRIORITY)

**Find files using removed databases:**

```bash
# Found 25 references:
# - blockchain.cpp (12 refs to getBlockchainDB)
# - explorer_db_service.cpp (13 refs to getBlockchainDB)
# - test_sqlite_system.cpp (3 refs to all three)
```

**Actions:**
- `blockchain.cpp` → DELETE file entirely (legacy SQLite wrapper, use ChainDB instead)
- `explorer_db_service.cpp` → UPDATE to query ChainDB directly (no more explorer.db)
- `test_sqlite_system.cpp` → UPDATE tests to reflect new architecture

---

### **Phase 4: BlockAcceptor Migration** (MEDIUM PRIORITY)

**Update BlockAcceptor to use GlobalUTXOSet:**

```cpp
// OLD (WRONG)
UTXOIndex* utxo_index = ctx.chainstate->getUTXOIndex();
utxo_index->AddUTXO(...);

// NEW (CORRECT)
GlobalUTXOSet* global_utxo = ctx.chainstate->getGlobalUTXOSet();
global_utxo->addUTXO(...);

// Notify wallet separately
if (wallet_owns_output) {
    WalletUTXO wallet_utxo = {txid, vout, derivation_path, amount, height};
    wallet->getUTXOTracker()->addOwnedUTXO(wallet_utxo);
}
```

---

### **Phase 5: Database Migration** (LOW PRIORITY)

**Migrate existing `~/.dinero/blockchain/utxo`:**

From: SQLite file
To: RocksDB directory

**Migration script:**
```bash
#!/bin/bash
# Stop daemon
./dinerod stop

# Backup old UTXO database
cp ~/.dinero/blockchain/utxo ~/.dinero/blockchain/utxo.sqlite.backup

# Remove old SQLite file
rm ~/.dinero/blockchain/utxo

# Create RocksDB directory
mkdir -p ~/.dinero/blockchain/utxo

# Start daemon (will populate GlobalUTXOSet from chaindb)
./dinerod -rescan
```

---

### **Phase 6: Mempool/Peers Implementation** (LOW PRIORITY)

**Mempool:**
- Create `src/daemon/mempool_manager.cpp` (RAM-only)
- Implement optional `mempool.dat` binary dump
- NO database (SQLite or RocksDB)

**Peers:**
- Create `src/daemon/peers_manager.cpp`
- Use binary `peers.dat` format
- NO SQLite database

---

## 🎯 **Final Architecture (Target State)**

### **Storage Layout:**
```
~/.dinero/
├── blockchain/
│   ├── chaindb/              ← RocksDB (block index)
│   ├── utxo/                 ← RocksDB (GlobalUTXOSet) ✅
│   └── blks/                 ← Flat files (raw blocks)
│
├── mempool.dat               ← Binary dump (optional) TODO
├── peers.dat                 ← Binary (peer addresses) TODO
│
└── wallets/
    └── {wallet_name}/
        └── wallet.db         ← SQLite (per-wallet)
            ├── wallet_meta       ← Wallet metadata
            ├── hd_seeds          ← HD wallet seed (encrypted)
            ├── addresses         ← HD addresses (BIP84/BIP86)
            ├── address_derivation_paths  ← Derivation path tracking
            ├── utxos             ← Legacy UTXO tracking (DEPRECATED)
            ├── wallet_utxos      ← NEW WalletUTXOTracker ✅
            ├── transactions      ← Tx history
            ├── ln_channels       ← Lightning channels ✅
            ├── ln_htlcs          ← Lightning HTLCs ✅
            ├── ln_commitments    ← Lightning commitments ✅
            ├── ln_peers          ← Lightning peers ✅
            ├── ln_invoices       ← Lightning invoices ✅
            ├── ln_payments       ← Lightning payments ✅
            └── ln_secrets        ← Lightning secrets ✅

MEMORY (NOT PERSISTED):
└── mempool                   ← std::unordered_map (RAM-only) TODO
```

### **Schema Files (Only One):**
```
resources/schema/
└── wallet_schema.sql         ← ONLY schema file needed
    ├── wallet tables
    ├── wallet_utxos (NEW - add this)
    └── ln_* tables (already present)
```

---

## ✅ **Success Criteria**

When refactor is complete:

1. ✅ GlobalUTXOSet (RocksDB) for consensus UTXOs
2. ✅ WalletUTXOTracker (SQLite) for wallet UTXOs
3. ✅ Lightning tables in wallet.db
4. ✅ Only `resources/schema/wallet_schema.sql` exists
5. ✅ No `database/schema/` directory
6. ✅ No explorer.db, mempool.db, peers.db
7. ✅ Mempool is RAM-only (no database)
8. ✅ `wallet_utxos` table in wallet_schema.sql
9. ✅ All code uses GlobalUTXOSet, not old UTXOIndex
10. ✅ All tests pass

---

## 📊 **Bitcoin Core Equivalence**

| Component | Bitcoin Core | DineroCoin | Status |
|-----------|--------------|------------|--------|
| Consensus UTXO | chainstate/ (LevelDB/RocksDB) | blockchain/utxo/ (RocksDB) | ✅ |
| Block Index | blocks/index/ (LevelDB) | blockchain/chaindb/ (RocksDB) | ✅ |
| Block Data | blocks/ (flat files) | blockchain/blks/ (flat files) | ✅ |
| Wallet | wallets/ (SQLite) | wallets/ (SQLite) | ✅ |
| Mempool | RAM-only (CTxMemPool) | RAM-only (MempoolManager) | TODO |
| Mempool Dump | mempool.dat (binary) | mempool.dat (binary) | TODO |
| Peers | peers.dat (binary) | peers.dat (binary) | TODO |
| Lightning | N/A | wallet.db (ln_* tables) | ✅ |

---

## 🔥 **Key Principles Learned**

1. **Only wallet data should use SQLite**
   - Wallet keys, addresses, metadata
   - Wallet UTXO tracking
   - Lightning state (wallet-specific)

2. **Consensus data must use RocksDB**
   - Global UTXO set
   - Block index (ChainDB)

3. **Mempool must be RAM-only**
   - No SQLite, no RocksDB
   - Optional binary dump for convenience
   - Changes thousands of times per second

4. **Separation of concerns**
   - GlobalUTXOSet = ALL blockchain UTXOs (consensus)
   - WalletUTXOTracker = THIS wallet's UTXOs (application)
   - No mixing of responsibilities

5. **Follow Bitcoin Core exactly**
   - Proven architecture
   - Battle-tested design
   - Industry standard

---

**Status:** Architecture clarified, schema conflicts identified, ready for implementation

**Next Action:** Delete conflicting schema files and fix wallet_utxos table definition
