# Database Schema Audit Report

**Date:** December 4, 2025
**Issue:** Multiple conflicting schema definitions found

---

## 🚨 **CRITICAL CONFLICTS FOUND**

### **Problem: Schema files exist for databases we removed**

We removed `explorer.db`, `mempool.db`, and `peers.db` from `sqlite_manager.cpp`, but their schema files still exist and are still referenced in the code!

---

## 📋 **Schema Files Inventory**

### **1. Explorer Schema (SHOULD NOT EXIST)**

**Files:**
- `database/schema/explorer_schema.sql` ❌ DELETE
- Referenced in `sqlite_manager.cpp:614-620` ❌ DELETE

**Problem:**
- Contains `utxo` table (lines 71-83) - conflicts with `GlobalUTXOSet` (RocksDB)
- Creates SQLite UTXO tracking (wrong - should use RocksDB)
- Creates block index in SQLite (wrong - should use ChainDB RocksDB)

**Action Required:**
```bash
rm database/schema/explorer_schema.sql
# Remove references in sqlite_manager.cpp (lines 614-620)
```

---

### **2. Mempool Schema (SHOULD NOT EXIST)**

**Files:**
- `database/schema/mempool_schema.sql` ❌ DELETE
- Referenced in `sqlite_manager.cpp:748-754` ❌ DELETE

**Problem:**
- Creates SQLite mempool (WRONG - mempool must be RAM-only!)
- Not Bitcoin Core compatible
- Bitcoin Core uses RAM-only mempool with optional mempool.dat binary dump

**Why This Is Wrong:**
```
❌ Mempool changes thousands of times per second
❌ Every tx arrival/confirmation/eviction would hit disk
❌ Massive write amplification and lock contention
❌ Bitcoin Core uses std::map in RAM, NOT a database
❌ Optional mempool.dat is a binary dump (NOT SQLite or RocksDB)
```

**Action Required:**
```bash
rm database/schema/mempool_schema.sql
# Remove references in sqlite_manager.cpp (lines 748-754)
# Create mempool_manager.cpp with RAM-only mempool
# Optionally: Add mempool.dat binary dump on shutdown
```

---

### **3. Peers Schema (SHOULD NOT EXIST)**

**Files:**
- `database/schema/peers_schema.sql` ❌ DELETE
- Referenced in `sqlite_manager.cpp:822-828` ❌ DELETE

**Problem:**
- Creates SQLite peers database (wrong - should be binary peers.dat or RocksDB)
- Not Bitcoin Core compatible

**Action Required:**
```bash
rm database/schema/peers_schema.sql
# Remove references in sqlite_manager.cpp (lines 822-828)
# Create peers_manager.cpp with binary storage
```

---

### **4. Wallet Schema (MULTIPLE VERSIONS - CONFLICT!)**

**Files:**
- `database/schema/wallet_schema.sql` (120 lines) ⚠️ OLD VERSION
- `resources/schema/wallet_schema.sql` (371 lines) ✅ CORRECT VERSION

**Comparison:**

| Feature | database/schema/ (OLD) | resources/schema/ (NEW) |
|---------|------------------------|-------------------------|
| **Size** | 120 lines | 371 lines |
| **wallet_utxos table** | ❌ NO | ✅ YES (lines 93-108) |
| **Lightning tables** | ❌ NO | ✅ YES (ln_channels, ln_htlcs, etc.) |
| **Encryption metadata** | ❌ NO | ✅ YES (Argon2id support) |
| **HD wallet support** | ✅ Basic | ✅ Complete (BIP84/BIP86) |

**Which is Used?**
- `wallet_manager.cpp:584` → uses `resources/schema/wallet_schema.sql` ✅ CORRECT
- `sqlite_manager.cpp:491` → uses `database/schema/wallet_schema.sql` ❌ WRONG!

**Conflict:**
- `WalletManager` uses new schema (with wallet_utxos + Lightning)
- `SQLiteManager` references old schema (missing wallet_utxos + Lightning)
- This creates schema inconsistency!

**Action Required:**
```bash
# Delete old schema
rm database/schema/wallet_schema.sql

# Update sqlite_manager.cpp:491 to use resources/schema/wallet_schema.sql
# OR: Remove wallet schema loading from sqlite_manager.cpp entirely (WalletManager handles it)
```

---

## 📊 **Schema Conflict Summary**

### **Resources/Schema Directory (CORRECT):**
```
resources/schema/
└── wallet_schema.sql          ✅ CORRECT (371 lines, includes wallet_utxos + Lightning)
```

**This schema includes:**
- ✅ `wallet_utxos` table (lines 93-108) - matches `WalletUTXOTracker`
- ✅ `ln_channels` table (lines 189-221) - Lightning channels
- ✅ `ln_htlcs` table (lines 227-250) - Lightning HTLCs
- ✅ `ln_commitments` table (lines 252-266) - Commitment txs
- ✅ `ln_peers` table (lines 268-276) - Lightning peers
- ✅ `ln_invoices` table (lines 278-306) - BOLT #11 invoices
- ✅ `ln_payments` table (lines 308-342) - Payment history
- ✅ `ln_secrets` table (lines 344-355) - Revocation secrets

### **Database/Schema Directory (WRONG - DELETE):**
```
database/schema/
├── explorer_schema.sql        ❌ DELETE (conflicts with GlobalUTXOSet)
├── mempool_schema.sql         ❌ DELETE (conflicts with RocksDB mempool)
├── peers_schema.sql           ❌ DELETE (conflicts with binary peers.dat)
└── wallet_schema.sql          ❌ DELETE (old version, missing wallet_utxos + Lightning)
```

---

## 🔍 **Verification: WalletUTXOTracker vs Schema**

### **WalletUTXOTracker Expected Schema:**
```cpp
// From wallet_utxo_tracker.cpp:36-49
CREATE TABLE IF NOT EXISTS wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,
    cached_amount INTEGER NOT NULL,
    cached_height INTEGER NOT NULL,
    is_spent INTEGER NOT NULL DEFAULT 0,
    is_locked INTEGER NOT NULL DEFAULT 0,
    label TEXT,
    PRIMARY KEY (txid, vout)
);
```

### **Actual Schema in resources/schema/wallet_schema.sql:**
```sql
-- Lines 93-108
CREATE TABLE IF NOT EXISTS utxos (
    id INTEGER PRIMARY KEY,
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    amount INTEGER NOT NULL,
    address TEXT NOT NULL,
    script_pubkey TEXT NOT NULL,
    height INTEGER NOT NULL DEFAULT 0,
    is_coinbase INTEGER NOT NULL DEFAULT 0,
    is_spent INTEGER NOT NULL DEFAULT 0,
    spent_txid TEXT,
    spent_height INTEGER,
    confirmations INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    UNIQUE(txid, vout)
);
```

### ⚠️ **SCHEMA MISMATCH DETECTED!**

**Problem:** The table names and columns don't match!

| Component | WalletUTXOTracker.cpp | wallet_schema.sql |
|-----------|----------------------|-------------------|
| **Table name** | `wallet_utxos` | `utxos` |
| **derivation_path** | ✅ YES | ❌ NO |
| **cached_amount** | ✅ YES | Uses `amount` |
| **cached_height** | ✅ YES | Uses `height` |
| **is_locked** | ✅ YES | ❌ NO |
| **label** | ✅ YES | ❌ NO |
| **address** | ❌ NO | ✅ YES |
| **script_pubkey** | ❌ NO | ✅ YES |

**This is a CRITICAL bug!** The `WalletUTXOTracker` class expects different columns than what the schema provides.

---

## 🔧 **Required Actions**

### **Phase 1: Delete Wrong Schemas**
```bash
cd /Users/haydarevich/Documents/DineroCoin

# Delete explorer, mempool, peers schemas (shouldn't exist)
rm database/schema/explorer_schema.sql
rm database/schema/mempool_schema.sql
rm database/schema/peers_schema.sql

# Delete old wallet schema (outdated)
rm database/schema/wallet_schema.sql

# Remove entire database/schema/ directory (no longer needed)
rmdir database/schema/
```

### **Phase 2: Fix sqlite_manager.cpp References**
Remove these code blocks:
- Lines 614-620: Explorer schema loading (DELETE)
- Lines 748-754: Mempool schema loading (DELETE)
- Lines 822-828: Peers schema loading (DELETE)
- Lines 491-498: Wallet schema loading (DELETE - WalletManager handles this)

### **Phase 3: Fix WalletUTXOTracker Schema Mismatch**

**Option A: Update wallet_schema.sql to match WalletUTXOTracker**
```sql
-- Add to resources/schema/wallet_schema.sql (after line 108)
CREATE TABLE IF NOT EXISTS wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,
    cached_amount INTEGER NOT NULL,
    cached_height INTEGER NOT NULL,
    is_spent INTEGER NOT NULL DEFAULT 0,
    is_locked INTEGER NOT NULL DEFAULT 0,
    label TEXT,
    PRIMARY KEY (txid, vout)
);
```

**Option B: Update WalletUTXOTracker to use existing `utxos` table**
- Rename `wallet_utxos` → `utxos` in code
- Add derivation path lookup from `address_derivation_paths` table
- Add `is_locked` column via ALTER TABLE migration

**Recommendation:** Option A (add new table) - cleaner separation between:
- `utxos` table: Legacy wallet UTXO tracking
- `wallet_utxos` table: New WalletUTXOTracker (matches GlobalUTXOSet architecture)

---

## ✅ **Success Criteria**

After fixes:
1. ✅ Only `resources/schema/wallet_schema.sql` exists
2. ✅ `wallet_schema.sql` includes `wallet_utxos` table matching `WalletUTXOTracker`
3. ✅ No `explorer_schema.sql`, `mempool_schema.sql`, `peers_schema.sql`
4. ✅ `sqlite_manager.cpp` doesn't reference removed schemas
5. ✅ `WalletUTXOTracker` creates correct schema on initialization
6. ✅ Lightning tables exist in wallet schema (already correct)

---

## 🎯 **Final Database Architecture**

### **Correct Layout:**
```
~/.dinero/
├── blockchain/
│   ├── chaindb/              ← RocksDB (block index)
│   ├── utxo/                 ← RocksDB (GlobalUTXOSet) NEW
│   └── blks/                 ← Flat files (raw blocks)
│
├── mempool/                  ← RocksDB (to be created)
├── peers.dat                 ← Binary (to be created)
│
└── wallets/
    └── {wallet_name}/
        └── wallet.db         ← SQLite (per-wallet)
            ├── wallet metadata
            ├── utxos (legacy)
            ├── wallet_utxos (NEW - matches WalletUTXOTracker)
            └── ln_* tables (Lightning)
```

### **Schema Files:**
```
resources/schema/
└── wallet_schema.sql         ← Only wallet schemas (ONLY file needed)
```

---

**Next Action:** Delete conflicting schema files and fix WalletUTXOTracker table mismatch
