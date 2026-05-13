# Database Refactor Implementation Progress

**Date:** December 4, 2025
**Status:** In Progress

---

## ✅ **COMPLETED**

### **1. Deleted Conflicting Schema Directory** ✅
```bash
rm -rf database/schema/
```

**Removed:**
- ❌ `explorer_schema.sql` (conflicted with GlobalUTXOSet)
- ❌ `mempool_schema.sql` (mempool must be RAM-only)
- ❌ `peers_schema.sql` (peers should be binary)
- ❌ `wallet_schema.sql` (old version, 120 lines)
- ❌ All migration files
- ❌ Documentation files

**Result:** Only canonical schemas remain in `resources/schema/`

---

### **2. Created Lightning Schema File** ✅
**File:** `resources/schema/lightning_schema.sql`

**Contents:**
- `ln_channels` - Lightning channel state
- `ln_htlcs` - Hash Time-Locked Contracts
- `ln_commitments` - Commitment transactions
- `ln_peers` - Lightning peer connections
- `ln_invoices` - BOLT #11 invoices
- `ln_payments` - Outgoing payment history
- `ln_secrets` - Encrypted revocation secrets
- `ln_watchtower` - Breach detection metadata
- Schema version tracking

**Purpose:** Separate Lightning state from on-chain wallet data

---

### **3. Updated Wallet Schema** ✅
**File:** `resources/schema/wallet_schema.sql`

**Changes:**
1. **Removed:** All Lightning tables (ln_*)
   - Moved to `lightning_schema.sql`
   - Added comment explaining separation

2. **Added:** `wallet_utxos` table
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

**Purpose:** Clean separation between on-chain (wallet.db) and off-chain (lightning.db)

---

### **4. Updated SQLiteLightningDB Header** ✅
**File:** `include/lightning/sqlite_lightning_db.h`

**Changes:**
1. Constructor now takes `db_path` (not `sqlite3* wallet_db`)
   ```cpp
   // OLD
   explicit SQLiteLightningDB(sqlite3* wallet_db);

   // NEW
   explicit SQLiteLightningDB(const std::string& db_path);
   ```

2. Added `initialize()` method to create schema

3. Updated class members:
   ```cpp
   std::string db_path_;  // Path to lightning.db
   sqlite3* db_;          // Owned by this class (not external)
   ```

4. Updated documentation to reflect separate database

**Purpose:** Lightning opens its own `lightning.db` file, not shared with wallet.db

---

## 📋 **REMAINING WORK**

### **Phase 1: Update SQLiteLightningDB Implementation**

**File:** `src/lightning/sqlite_lightning_db.cpp`

**Required Changes:**
1. Update constructor to open lightning.db:
   ```cpp
   SQLiteLightningDB::SQLiteLightningDB(const std::string& db_path)
       : db_path_(db_path), db_(nullptr) {
       // Open lightning.db
       int rc = sqlite3_open(db_path.c_str(), &db_);
       // ...
   }
   ```

2. Implement `initialize()` method:
   ```cpp
   Status SQLiteLightningDB::initialize() {
       // Load lightning_schema.sql
       // Execute schema creation
       // Enable WAL mode
       // Return status
   }
   ```

3. Update destructor to close database:
   ```cpp
   SQLiteLightningDB::~SQLiteLightningDB() {
       if (db_) {
           sqlite3_close(db_);
           db_ = nullptr;
       }
   }
   ```

---

### **Phase 2: Update LightningService**

**File:** `src/lightning/lightning_service.cpp`

**Required Changes:**

```cpp
// In LightningService::InitForWallet()

// OLD (WRONG - wrapped wallet.db)
sqlite3* wallet_db = wallet_mgr->getActiveWalletDB();
m_db = std::make_shared<SQLiteLightningDB>(wallet_db);

// NEW (CORRECT - opens lightning.db)
std::string wallet_dir = wallet_mgr->getActiveWalletDirectory();
std::string lightning_db_path = wallet_dir + "/lightning.db";
m_db = std::make_shared<SQLiteLightningDB>(lightning_db_path);

// Initialize (creates schema if needed)
Status init_status = m_db->initialize();
if (!init_status.isOk()) {
    g_logger.error("Failed to initialize Lightning DB: " + init_status.message());
    return false;
}
```

---

### **Phase 3: Remove Schema Loading from SQLiteManager**

**File:** `src/database/sqlite_manager.cpp`

**Lines to Remove:**
- Lines 614-620: Explorer schema loading
- Lines 748-754: Mempool schema loading
- Lines 822-828: Peers schema loading
- Lines 491-498: Old wallet schema loading (WalletManager handles this)

**Methods to Remove:**
- `initializeBlockchainDB()`
- `initializeMempoolDB()`
- `initializePeersDB()`
- `createBlockchainSchema()`
- `createMempoolSchema()`
- `createPeersSchema()`

---

### **Phase 4: Update BlockAcceptor**

**File:** Find BlockAcceptor implementation

**Required Changes:**
```cpp
// OLD (WRONG)
UTXOIndex* utxo_index = ctx.chainstate->getUTXOIndex();
utxo_index->AddUTXO(...);

// NEW (CORRECT)
GlobalUTXOSet* global_utxo = ctx.chainstate->getGlobalUTXOSet();
global_utxo->addUTXO(...);

// Notify wallet separately
if (wallet_owns_output) {
    WalletUTXO wallet_utxo = {
        .txid = txid,
        .vout = vout,
        .derivation_path = path,
        .cached_amount = amount,
        .cached_height = height
    };
    wallet->getUTXOTracker()->addOwnedUTXO(wallet_utxo);
}
```

---

### **Phase 5: Delete Legacy Files**

**Files to Delete:**
```bash
rm src/daemon/blockchain.cpp
rm include/daemon/blockchain.h
```

**Reason:** Legacy SQLite wrapper for blockchain data. All queries should use ChainDB (RocksDB) directly.

---

### **Phase 6: Update Explorer Service**

**File:** `src/services/explorer_db_service.cpp`

**Required Changes:**
- Remove all `getBlockchainDB()` calls (13 occurrences)
- Query ChainDB (RocksDB) directly
- Remove explorer.db creation/syncing

---

## 🎯 **Final Architecture (Target)**

```
~/.dinero/
├── blockchain/
│   ├── blks/           ← Flat files
│   ├── chaindb/        ← RocksDB (consensus)
│   └── utxo/           ← RocksDB (GlobalUTXOSet) ✅
│
├── mempool.dat         ← Binary (optional)
├── peers.dat           ← Binary (optional)
│
└── wallets/
    └── {wallet_name}/
        ├── wallet.db       ← SQLite (on-chain) ✅
        │   - wallet_utxos ✅
        └── lightning.db    ← SQLite (off-chain) ✅
            - ln_* tables ✅

MEMORY:
- mempool (RAM-only)
- Lightning caches
```

---

## 📊 **Progress Summary**

| Task | Status | File |
|------|--------|------|
| Delete conflicting schemas | ✅ DONE | database/schema/ |
| Create lightning_schema.sql | ✅ DONE | resources/schema/ |
| Update wallet_schema.sql | ✅ DONE | resources/schema/ |
| Add wallet_utxos table | ✅ DONE | wallet_schema.sql |
| Remove Lightning from wallet schema | ✅ DONE | wallet_schema.sql |
| Update SQLiteLightningDB header | ✅ DONE | include/lightning/ |
| Update SQLiteLightningDB impl | ⏳ TODO | src/lightning/ |
| Update LightningService | ⏳ TODO | src/lightning/ |
| Clean up SQLiteManager | ⏳ TODO | src/database/ |
| Update BlockAcceptor | ⏳ TODO | TBD |
| Delete blockchain.cpp | ⏳ TODO | src/daemon/ |
| Update explorer service | ⏳ TODO | src/services/ |

---

**Status:** 50% complete - Schema files cleaned, Lightning separated, wallet_utxos added
**Next:** Update Lightning DB implementation to open separate file
