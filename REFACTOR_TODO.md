# Database Refactor TODO

**Date:** December 4, 2025
**Status:** In Progress - SQLite databases removed, code needs updating

---

## ✅ **COMPLETED**

1. ✅ Created `consensus/global_utxo_set.h/cpp` - Global UTXO set (RocksDB)
2. ✅ Created `wallet/wallet_utxo_tracker.h/cpp` - Per-wallet UTXO tracking (SQLite)
3. ✅ Created `ARCHITECTURE_DB.md` - Architecture documentation
4. ✅ Removed `explorer.db`, `mempool.db`, `peers.db` from `sqlite_manager.h/cpp`

---

## 🔧 **FILES THAT NEED UPDATING**

### **1. Blockchain.cpp (12 occurrences)**
**File:** `src/daemon/blockchain.cpp`
**Problem:** Uses `db_manager_->getBlockchainDB()` (removed method)
**Solution:** Should use ChainDB (RocksDB) instead of SQLite

**Lines:**
- 73: `sqlite3* blockchain_db = db_manager_->getBlockchainDB();`
- 173, 267, 370, 390, 430, 447, 464, 484, 551, 701, 772

**Action Required:**
- **Option A:** Update Blockchain class to use ChainDB directly
- **Option B:** Mark these methods as deprecated and route to ChainDB
- **Option C:** Remove SQLite Blockchain entirely (use ChainDB only)

**Recommendation:** Option C - Blockchain.cpp is a legacy SQLite wrapper that duplicates ChainDB functionality

---

### **2. ExplorerDBService.cpp (13 occurrences)**
**File:** `src/services/explorer_db_service.cpp`
**Problem:** Uses `db_manager_->getBlockchainDB()` (removed method)
**Solution:** Should query ChainDB (RocksDB) directly

**Lines:**
- 85, 116, 165, 198, 224, 253, 335, 391, 452, 486, 548

**Action Required:**
- Update ExplorerDBService to query ChainDB instead of SQLite
- Remove explorer.db references entirely
- Use ChainDB as single source of truth

**Note:** ExplorerDBService was syncing from ChainDB to explorer.db (redundant)

---

## 🗑️ **FILES TO DELETE**

### **SQLite Blockchain Wrapper (Legacy)**
```
src/daemon/blockchain.cpp         ← DELETE (use ChainDB instead)
include/daemon/blockchain.h       ← DELETE (use ChainDB instead)
```

**Reason:** This is a SQLite wrapper around blockchain data. With the refactor, all blockchain queries should go directly to ChainDB (RocksDB), not through a SQLite mirror.

---

### **Explorer DB Service (Redundant)**
```
src/services/explorer_db_service.cpp    ← DELETE or UPDATE to use ChainDB
include/services/explorer_db_service.h  ← DELETE or UPDATE to use ChainDB
```

**Reason:** This service was syncing ChainDB → explorer.db → queries. Now queries should go directly to ChainDB.

**Alternative:** Keep ExplorerDBService but have it query ChainDB directly (no more explorer.db).

---

## 📋 **REMAINING TASKS**

### **Phase 1: Code Cleanup** ⏳
- [ ] Grep for all `getBlockchainDB()` references
- [ ] Grep for all `getMempoolDB()` references
- [ ] Grep for all `getPeersDB()` references
- [ ] Update or delete files using these methods

### **Phase 2: Architecture Changes** ⏳
- [ ] Delete `src/daemon/blockchain.cpp` (legacy SQLite wrapper)
- [ ] Update `ExplorerDBService` to use ChainDB directly
- [ ] Remove `explorer.db` creation code
- [ ] Verify all queries go to ChainDB (RocksDB)

### **Phase 3: BlockAcceptor Update** ⏳
- [ ] Update BlockAcceptor to use `GlobalUTXOSet` instead of old `UTXOIndex`
- [ ] Test block processing with new UTXO set
- [ ] Verify atomic batch operations work

### **Phase 4: Wallet Integration** ⏳
- [ ] Integrate `WalletUTXOTracker` into wallet code
- [ ] Test wallet balance queries
- [ ] Test wallet transaction creation
- [ ] Verify wallet rescan functionality

### **Phase 5: Database Migration** ⏳
- [ ] Create migration script for existing `~/.dinero/blockchain/utxo` (SQLite → RocksDB)
- [ ] Test migration with production data
- [ ] Verify UTXO counts match before/after

### **Phase 6: Mempool/Peers** ⏳
- [ ] Create `mempool_manager.cpp` (RAM-only mempool, like Bitcoin Core)
- [ ] Implement optional `mempool.dat` binary dump (convenience feature)
- [ ] Create `peers_manager.cpp` (Binary `peers.dat` storage)
- [ ] Verify mempool NEVER writes to disk during operation

---

## 🚨 **BREAKING CHANGES**

### **API Changes:**
```cpp
// OLD (REMOVED)
sqlite3* db = db_manager_->getBlockchainDB();
sqlite3* db = db_manager_->getMempoolDB();
sqlite3* db = db_manager_->getPeersDB();

// NEW (CORRECT)
ChainDB* chaindb = daemon_context->chainstate->getChainDB();
GlobalUTXOSet* utxo_set = daemon_context->chainstate->getGlobalUTXOSet();
WalletUTXOTracker* wallet_utxos = wallet->getUTXOTracker();
```

### **Database Files Removed:**
```bash
~/.dinero/explorer.db      ← DELETE (redundant, use ChainDB)
~/.dinero/mempool.db       ← DELETE (use RocksDB instead)
~/.dinero/peers.db         ← DELETE (use binary format)
~/.dinero/blockchain/utxo  ← MIGRATE (SQLite file → RocksDB directory)
```

### **Database Files Remaining:**
```bash
~/.dinero/blockchain/chaindb/     ← RocksDB (block index)
~/.dinero/blockchain/utxo/        ← RocksDB (global UTXO set) NEW
~/.dinero/wallets/{name}/wallet.db ← SQLite (wallet data) ONLY
```

---

## 🔍 **GREP COMMANDS FOR CLEANUP**

### **Find all references:**
```bash
# Find getBlockchainDB() usage
grep -r "getBlockchainDB" src/ --include="*.cpp" -n

# Find getMempoolDB() usage
grep -r "getMempoolDB" src/ --include="*.cpp" -n

# Find getPeersDB() usage
grep -r "getPeersDB" src/ --include="*.cpp" -n

# Find explorer.db references
grep -r "explorer\.db" src/ --include="*.cpp" -n

# Find mempool.db references
grep -r "mempool\.db" src/ --include="*.cpp" -n

# Find peers.db references
grep -r "peers\.db" src/ --include="*.cpp" -n

# Find old UTXOIndex usage
grep -r "UTXOIndex" src/ --include="*.cpp" -n
```

---

## 📊 **IMPACT ANALYSIS**

### **Files Affected:**
- `src/daemon/blockchain.cpp` (12 references) → DELETE or UPDATE
- `src/services/explorer_db_service.cpp` (13 references) → UPDATE
- `src/database/sqlite_manager.h/cpp` → ✅ UPDATED
- All code using `UTXOIndex` → UPDATE to `GlobalUTXOSet` + `WalletUTXOTracker`

### **Tests Affected:**
- Any tests using `getBlockchainDB()` → UPDATE
- Any tests using SQLite UTXO → UPDATE
- Integration tests → UPDATE to use new architecture

---

## ✅ **SUCCESS CRITERIA**

1. ✅ No `explorer.db`, `mempool.db`, `peers.db` created
2. ✅ All queries use ChainDB (RocksDB) directly
3. ✅ Wallet uses `WalletUTXOTracker` (SQLite per-wallet)
4. ✅ Consensus uses `GlobalUTXOSet` (RocksDB)
5. ✅ No SQLite for consensus data
6. ✅ Bitcoin Core-compatible architecture
7. ✅ All tests pass with new architecture

---

**Next Action:** Update or delete `src/daemon/blockchain.cpp` (legacy SQLite wrapper)
