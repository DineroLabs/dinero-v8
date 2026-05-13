# Schema Consistency Verification

**Date:** December 4, 2025
**Status:** Checking for conflicts between WalletUTXOTracker and wallet_schema.sql

---

## 🔍 **Issue: Two UTXO Tables in Wallet Database**

### **Table 1: `utxos` (from wallet_schema.sql)**

**Location:** `resources/schema/wallet_schema.sql:93-108`

```sql
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

**Purpose:** Legacy wallet UTXO tracking
**Created by:** Wallet initialization (wallet_schema.sql)
**Used by:** Old wallet code (?)

---

### **Table 2: `wallet_utxos` (from WalletUTXOTracker)**

**Location:** `src/wallet/wallet_utxo_tracker.cpp:14-24`

```sql
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

**Purpose:** New WalletUTXOTracker (Bitcoin Core-compatible)
**Created by:** WalletUTXOTracker::initialize()
**Used by:** New GlobalUTXOSet + WalletUTXOTracker architecture

---

## 📊 **Column Comparison**

| Column | `utxos` (old) | `wallet_utxos` (new) | Notes |
|--------|---------------|---------------------|-------|
| **txid** | ✅ TEXT | ✅ TEXT | ✅ Match |
| **vout** | ✅ INTEGER | ✅ INTEGER | ✅ Match |
| **derivation_path** | ❌ NO | ✅ TEXT | NEW (wallet-specific) |
| **amount** | ✅ (direct) | ✅ (cached_amount) | ⚠️ Different name |
| **height** | ✅ (direct) | ✅ (cached_height) | ⚠️ Different name |
| **is_spent** | ✅ INTEGER | ✅ INTEGER | ✅ Match |
| **is_locked** | ❌ NO | ✅ INTEGER | NEW (coin control) |
| **label** | ❌ NO | ✅ TEXT | NEW (user labels) |
| **address** | ✅ TEXT | ❌ NO | OLD (can derive from derivation_path) |
| **script_pubkey** | ✅ TEXT | ❌ NO | OLD (can get from GlobalUTXOSet) |
| **is_coinbase** | ✅ INTEGER | ❌ NO | OLD (can get from GlobalUTXOSet) |
| **spent_txid** | ✅ TEXT | ❌ NO | OLD (not needed) |
| **spent_height** | ✅ INTEGER | ❌ NO | OLD (not needed) |
| **confirmations** | ✅ INTEGER | ❌ NO | OLD (computed on-demand) |

---

## ⚠️ **Potential Conflicts**

### **1. Two Tables for Same Purpose**

**Problem:**
- `utxos` table (legacy) stores wallet UTXOs
- `wallet_utxos` table (new) stores wallet UTXOs
- **Risk of inconsistency** if both are used

**Questions:**
1. Does old wallet code still use `utxos` table?
2. Does new code use `wallet_utxos` table?
3. Can they coexist, or must we migrate?

---

### **2. Schema Evolution Path**

**Option A: Coexist (Dual Schema)**
```
wallet.db:
├── utxos             ← Legacy table (keep for old code)
└── wallet_utxos      ← New table (used by WalletUTXOTracker)
```
**Pros:** No breaking changes
**Cons:** Data duplication, potential inconsistency

---

**Option B: Migrate (Single Schema)**
```
wallet.db:
└── wallet_utxos      ← Only table (migrate from utxos)
```
**Pros:** No duplication, clean architecture
**Cons:** Breaking change, requires migration script

---

**Option C: Extend Existing (Modify Schema)**
```sql
ALTER TABLE utxos ADD COLUMN derivation_path TEXT;
ALTER TABLE utxos ADD COLUMN is_locked INTEGER DEFAULT 0;
ALTER TABLE utxos ADD COLUMN label TEXT;
```
**Pros:** No table rename, incremental migration
**Cons:** Keeps old columns (address, script_pubkey, etc.)

---

## ✅ **Recommendation: Option B (Migrate to wallet_utxos)**

### **Why:**
1. Clean separation of concerns
2. `wallet_utxos` matches GlobalUTXOSet architecture
3. No redundant columns (address/script_pubkey in GlobalUTXOSet)
4. Follows Bitcoin Core model (wallet tracks ownership, not full UTXO data)

### **Migration Path:**

```sql
-- Step 1: Create wallet_utxos table (WalletUTXOTracker does this)
CREATE TABLE IF NOT EXISTS wallet_utxos (...);

-- Step 2: Migrate data from utxos → wallet_utxos
INSERT INTO wallet_utxos (txid, vout, derivation_path, cached_amount, cached_height, is_spent, is_locked, label)
SELECT
    txid,
    vout,
    (SELECT derivation_path FROM address_derivation_paths WHERE address = utxos.address) AS derivation_path,
    amount AS cached_amount,
    height AS cached_height,
    is_spent,
    0 AS is_locked,  -- Default: not locked
    NULL AS label    -- Default: no label
FROM utxos
WHERE is_spent = 0;  -- Only migrate unspent

-- Step 3: Drop old table (after verification)
DROP TABLE utxos;
```

---

## 🔧 **Implementation Plan**

### **Phase 1: Add wallet_utxos to wallet_schema.sql** ✅

Add `wallet_utxos` table definition to `resources/schema/wallet_schema.sql`:

```sql
-- Wallet UTXO Tracker (NEW - Bitcoin Core-compatible)
-- Tracks which UTXOs from GlobalUTXOSet belong to this wallet
CREATE TABLE IF NOT EXISTS wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,          -- "m/84'/1447'/0'/0/5"
    cached_amount INTEGER NOT NULL,         -- Cached from GlobalUTXOSet
    cached_height INTEGER NOT NULL,         -- Cached from GlobalUTXOSet
    is_spent INTEGER NOT NULL DEFAULT 0,    -- Spent by this wallet
    is_locked INTEGER NOT NULL DEFAULT 0,   -- Locked for coin control
    label TEXT,                             -- User label
    PRIMARY KEY (txid, vout)
);

CREATE INDEX IF NOT EXISTS idx_wallet_utxos_unspent ON wallet_utxos(is_spent) WHERE is_spent = 0;
CREATE INDEX IF NOT EXISTS idx_wallet_utxos_path ON wallet_utxos(derivation_path);
CREATE INDEX IF NOT EXISTS idx_wallet_utxos_height ON wallet_utxos(cached_height);
```

### **Phase 2: Mark utxos table as deprecated**

Add comment to wallet_schema.sql:

```sql
-- Legacy UTXO table (DEPRECATED - use wallet_utxos instead)
-- Kept for backward compatibility during migration
CREATE TABLE IF NOT EXISTS utxos (
    ...
);
```

### **Phase 3: Create migration script**

`scripts/migrate_wallet_utxos.sql`:

```sql
-- Migrate from legacy utxos → wallet_utxos
INSERT OR IGNORE INTO wallet_utxos (txid, vout, derivation_path, cached_amount, cached_height, is_spent, is_locked, label)
SELECT
    u.txid,
    u.vout,
    COALESCE(adp.derivation_path, 'unknown') AS derivation_path,
    u.amount AS cached_amount,
    u.height AS cached_height,
    u.is_spent,
    0 AS is_locked,
    NULL AS label
FROM utxos u
LEFT JOIN address_derivation_paths adp ON u.address = adp.address;
```

### **Phase 4: Update wallet code to use wallet_utxos**

Ensure all wallet code uses `WalletUTXOTracker` (which uses `wallet_utxos`), not direct `utxos` queries.

### **Phase 5: Remove utxos table (future release)**

After confirmation that no code uses `utxos`:
```sql
DROP TABLE utxos;
```

---

## 🎯 **Lightning Tables (No Conflicts)**

Lightning tables (`ln_channels`, `ln_htlcs`, etc.) are already in `wallet_schema.sql` and have no conflicts. ✅

---

## ✅ **Summary**

### **Current State:**
- ⚠️ Two UTXO tables coexist: `utxos` (legacy) and `wallet_utxos` (new)
- ⚠️ `WalletUTXOTracker` creates `wallet_utxos` dynamically
- ⚠️ `wallet_schema.sql` only has `utxos` (missing `wallet_utxos`)

### **Target State:**
- ✅ Add `wallet_utxos` to `wallet_schema.sql`
- ✅ Mark `utxos` as deprecated
- ✅ Provide migration script
- ✅ Eventually remove `utxos` table

### **Action Items:**
1. Add `wallet_utxos` table to `resources/schema/wallet_schema.sql`
2. Add migration comments to wallet_schema.sql
3. Create `scripts/migrate_wallet_utxos.sql`
4. Test migration with existing wallets
5. Update ARCHITECTURE_DB.md with final schema

---

**Status:** Ready to implement migration
