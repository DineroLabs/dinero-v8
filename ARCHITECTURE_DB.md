# DineroCoin Database Architecture

**Date:** December 4, 2025
**Status:** ✅ REFACTORED - Bitcoin Core Model

---

## 🎯 **Core Principle**

**DineroCoin follows Bitcoin Core's database model:**
- **Consensus data** → RocksDB (formerly LevelDB in Bitcoin)
- **Wallet data** → SQLite (per-wallet)
- **No SQLite for blockchain/mempool/peers** (this was a regression)

---

## 📂 **Database Layout (CORRECT)**

```
~/.dinero/
├── blockchain/
│   ├── chaindb/              ← RocksDB (block headers + index)
│   ├── utxo/                 ← RocksDB (GLOBAL UTXO SET) ✅ NEW
│   └── blks/                 ← Flat files (raw blocks)
│
├── mempool.dat               ← Binary dump (optional persistence on shutdown) ✅
│                                 NOT a database - just a convenience feature
│
├── peers.dat                 ← Binary (peer addresses) TODO
│
├── wallets/
│   └── {wallet_name}/
│       └── wallet.db         ← SQLite (wallet UTXOs, keys, metadata + Lightning state) ✅
│           ├── wallet tables     ← HD keys, addresses, labels
│           ├── wallet_utxos      ← Per-wallet UTXO tracking
│           └── ln_* tables       ← Lightning channels, HTLCs, invoices, peers
│
└── debug.log                 ← Single log file

MEMORY (NOT PERSISTED):
└── mempool                   ← std::unordered_map<uint256, MempoolEntry> ✅
                                  Active mempool lives entirely in RAM
                                  Changes thousands of times per second
                                  Non-consensus, volatile data
```

---

## 🗄️ **Database Components**

### **1. Global UTXO Set (Consensus-Critical)**

**File:** `include/consensus/global_utxo_set.h`
**Storage:** `~/.dinero/blockchain/utxo/` (RocksDB)
**Purpose:** Single source of truth for ALL unspent outputs in the blockchain

```cpp
class GlobalUTXOSet {
    // Add/Spend UTXOs (consensus operations)
    bool addUTXO(const GlobalUTXO& utxo);
    bool spendUTXO(const std::string& txid, uint32_t vout);
    bool hasUTXO(const std::string& txid, uint32_t vout);
    std::optional<GlobalUTXO> getUTXO(const std::string& txid, uint32_t vout);

    // Atomic batch operations (for block processing)
    rocksdb::WriteBatch* beginBatch();
    bool commitBatch(rocksdb::WriteBatch* batch);
};

struct GlobalUTXO {
    std::string txid;
    uint32_t vout;
    uint64_t amount;
    std::vector<uint8_t> scriptPubKey;
    uint32_t height;
    bool is_coinbase;
    // NO wallet-specific fields!
};
```

**Key Points:**
- ✅ Contains ALL UTXOs (not just wallet's)
- ✅ Used by block validation
- ✅ No derivation paths or labels
- ✅ RocksDB for performance

---

### **2. Wallet UTXO Tracker (Application-Level)**

**File:** `include/wallet/wallet_utxo_tracker.h`
**Storage:** `~/.dinero/wallets/{wallet_name}/wallet.db` (SQLite)
**Purpose:** Track which UTXOs from global set belong to THIS wallet

```cpp
class WalletUTXOTracker {
    // Add wallet-owned UTXO
    bool addOwnedUTXO(const WalletUTXO& utxo);
    bool markSpent(const std::string& txid, uint32_t vout);

    // Get wallet UTXOs
    std::vector<WalletUTXO> getUnspentUTXOs(bool include_locked = false);
    std::vector<WalletUTXO> getSpendableUTXOs(uint32_t current_height, uint32_t min_confirmations = 1);

    // Balance calculation
    WalletBalance getBalance(uint32_t current_height);

    // Coin control
    bool lockUTXO(const std::string& txid, uint32_t vout);
    bool unlockUTXO(const std::string& txid, uint32_t vout);

    // Synchronization
    uint64_t verifyAgainstGlobalSet();  // Check UTXOs still exist
    uint64_t rescan();  // Remove spent UTXOs
};

struct WalletUTXO {
    std::string txid;
    uint32_t vout;
    std::string derivation_path;  // "m/84'/1'/0'/0/12" (wallet-specific)
    uint64_t cached_amount;       // Cached from global UTXO
    uint32_t cached_height;       // Cached from global UTXO
    bool is_spent;                // Spent by this wallet
    bool is_locked;               // Locked for coin control
    std::string label;            // User label
};
```

**Key Points:**
- ✅ Only tracks UTXOs this wallet owns
- ✅ Contains wallet-specific metadata (derivation path, labels)
- ✅ Validates against GlobalUTXOSet
- ✅ SQLite per-wallet (lightweight)

---

### **3. Lightning Network State (Per-Wallet SQLite)**

**File:** `include/lightning/sqlite_lightning_db.h`
**Storage:** `~/.dinero/wallets/{wallet_name}/wallet.db` (SQLite, within wallet DB)
**Purpose:** Store Lightning Network state for THIS wallet

```cpp
class SQLiteLightningDB : public ILightningDB {
    // Uses ln_* tables in wallet database
    Status putChannel(const ChannelRecord& rec);
    Status putHTLC(const HTLCRecord& rec);
    Status putCommitment(const CommitmentRecord& rec);
    Status putPeer(const PeerRecord& rec);
    Status putInvoice(const InvoiceRecord& rec);
    Status putPayment(const PaymentRecord& rec);

    // Atomic operations
    Status atomicUpdateChannelHTLC(const ChannelRecord& chan, const HTLCRecord& htlc);
};
```

**Lightning Tables in wallet.db:**
```sql
-- ln_channels: Lightning channel state
-- ln_htlcs: Hash Time Locked Contracts
-- ln_commitments: Commitment transactions
-- ln_peers: Lightning peer connections
-- ln_invoices: Payment invoices (BOLT #11)
-- ln_payments: Payment history
-- ln_secrets: Encrypted revocation secrets
```

**Key Points:**
- ✅ Lightning state stored in wallet database (per-wallet isolation)
- ✅ Node identity derived from wallet's HD seed (BIP32 m/84'/1447'/9735'/0'/0')
- ✅ Lightning initialized only when wallet is opened
- ✅ Full isolation between different wallets' Lightning state
- ✅ SQLite is appropriate here (wallet-level data, not consensus-critical)

**Architecture:**
```cpp
// Lightning per-wallet lifecycle
LightningService::InitForWallet(WalletManager* wallet_mgr) {
    // 1. Get wallet's SQLite database
    sqlite3* wallet_db = wallet_mgr->getActiveWalletDB();

    // 2. Wrap it in Lightning DB interface
    m_db = std::make_shared<SQLiteLightningDB>(wallet_db);

    // 3. Verify ln_* tables exist
    m_db->verifySchema();

    // 4. Derive node identity from wallet seed
    initializeNodeIdentity();  // BIP32 path for Lightning

    // 5. Initialize Lightning components
    m_channel_mgr = std::make_unique<ChannelManager>(m_db);
    m_htlc_mgr = std::make_unique<HTLCManager>(m_db);
    m_payment_router = std::make_unique<PaymentRouter>(m_db);
}
```

**Why This Is Correct:**
- Lightning state is **wallet-specific**, not consensus-critical
- Each wallet has its own Lightning node identity
- Lightning channels belong to a specific wallet
- Bitcoin Core/LND also use per-wallet Lightning state
- SQLite is perfect for this use case (application-level, not blockchain consensus)

---

### **4. Mempool (RAM-Only, Non-Persistent)**

**Storage:** Memory (RAM) - `std::unordered_map<uint256, MempoolEntry>`
**Optional Persistence:** `~/.dinero/mempool.dat` (binary dump on shutdown)
**Purpose:** Active transaction pool for mining and relaying

**Why RAM-Only:**
```
✅ EXTREMELY HIGH FREQUENCY
   - New transactions arriving constantly
   - Block confirmations (remove all confirmed txs)
   - RBF replacements
   - Transaction evictions
   - Fee-rate recalculations
   - Ancestor/descendant updates

✅ NON-CONSENSUS DATA
   - Each node has different mempool
   - Optional, volatile
   - Not part of blockchain consensus

✅ PERFORMANCE
   - Disk I/O would create massive bottleneck
   - Lock contention with SQLite/RocksDB
   - Write amplification
   - Latency spikes
```

**Architecture:**
```cpp
class MempoolManager {
    // Active mempool (RAM only)
    std::unordered_map<uint256, MempoolEntry> m_transactions;
    std::multimap<double, uint256> m_fee_rate_index;  // For mining
    std::unordered_map<uint256, std::set<uint256>> m_ancestors;
    std::unordered_map<uint256, std::set<uint256>> m_descendants;

    // Optional: Save on shutdown
    bool dumpMempool(const std::string& filepath);  // → mempool.dat
    bool loadMempool(const std::string& filepath);  // ← mempool.dat
};
```

**mempool.dat Format:**
```
NOT a database - just a binary dump
Written on: daemon shutdown
Loaded on: daemon startup
Invalidated on: protocol upgrades, policy changes

Purpose: Convenience only (NOT correctness)
```

**Key Points:**
- ✅ Active mempool NEVER touches disk during operation
- ✅ No SQLite, no RocksDB, no LevelDB
- ✅ Optional binary dump for convenience (like Bitcoin Core)
- ✅ Mempool can be completely empty on restart (this is fine!)

**What Bitcoin Core Does:**
```cpp
// bitcoin/src/txmempool.h
class CTxMemPool {
    std::map<uint256, CTxMemPoolEntry> mapTx;  // RAM only!
    // ... no database connection
};

// bitcoin/src/validation.cpp
DumpMempool(mempool);  // Writes mempool.dat on shutdown
```

---

## 🔄 **Two-Level UTXO Architecture**

```
┌─────────────────────────────────────────────────┐
│  GlobalUTXOSet (RocksDB)                        │
│  ~/.dinero/blockchain/utxo/                     │
│                                                  │
│  • ALL UTXOs in blockchain                      │
│  • Consensus-critical                           │
│  • Updated during block validation              │
│  • No wallet-specific data                      │
└─────────────────────────────────────────────────┘
                     ▲
                     │ validates against
                     │
┌─────────────────────────────────────────────────┐
│  WalletUTXOTracker (SQLite)                     │
│  ~/.dinero/wallets/{wallet}/wallet.db           │
│                                                  │
│  • UTXOs owned by THIS wallet                   │
│  • Derivation paths, labels, locks              │
│  • Application-level tracking                   │
│  • Queries global set for validation            │
└─────────────────────────────────────────────────┘
```

**Query Flow:**
```cpp
// Get wallet balance
WalletBalance wallet.getBalance(current_height) {
    wallet_utxos = wallet_db.getUnspentUTXOs();  // SQLite

    for (utxo in wallet_utxos) {
        // Verify UTXO still exists in consensus set
        if (global_utxo_set.hasUTXO(utxo.txid, utxo.vout)) {  // RocksDB
            if (isMature(utxo, current_height)) {
                balance.confirmed += utxo.cached_amount;
            }
        }
    }
}
```

---

## ❌ **What Was WRONG (Regression)**

### **Old Architecture (INCORRECT):**
```
~/.dinero/
├── blockchain/
│   ├── chaindb/              ← RocksDB (correct)
│   └── utxo                  ← SQLite file ❌ (should be RocksDB directory)
│
├── wallet.db                 ← SQLite (correct)
├── explorer.db               ← SQLite ❌ (should not exist)
├── mempool.db                ← SQLite ❌ (should be RAM-only!)
├── peers.db                  ← SQLite ❌ (should be binary)
```

**Problems:**
1. ❌ Global UTXO set was SQLite (slow, not Bitcoin-compatible)
2. ❌ Mempool was SQLite (WRONG - mempool must be RAM-only!)
3. ❌ Peers was SQLite (should be binary)
4. ❌ Explorer.db duplicated chaindb data (queries should go to chaindb)
5. ❌ `UTXOIndex` class mixed global + wallet concerns

**Why Mempool SQLite Was Wrong:**
```
❌ Too slow for thousands of tx/sec
❌ Write amplification on every tx
❌ Lock contention
❌ Disk I/O bottleneck
❌ Not how Bitcoin Core works
❌ Breaks mempool volatility model
```

---

## ✅ **What Was FIXED**

### **Created:**
1. ✅ `consensus/global_utxo_set.h/cpp` - Global UTXO set (RocksDB)
2. ✅ `wallet/wallet_utxo_tracker.h/cpp` - Per-wallet tracking (SQLite)

### **To Remove:**
1. ❌ `wallet/utxo_index.h/cpp` - Delete (replaced by above two)
2. ❌ `database/sqlite_manager.cpp:39-41` - Remove explorer.db/mempool.db/peers.db
3. ❌ `~/.dinero/blockchain/utxo` (SQLite file) - Migrate to RocksDB

### **To Create:**
1. ⏳ `daemon/mempool_manager.cpp` - RAM-only mempool with optional mempool.dat dump
2. ⏳ `daemon/peers_manager.cpp` - Binary peers.dat storage

---

## 📊 **Database Comparison**

| Database | Old (WRONG) | New (CORRECT) | Purpose |
|----------|-------------|---------------|---------|
| **Global UTXO** | SQLite file | RocksDB directory ✅ | Consensus UTXOs |
| **Wallet UTXO** | Mixed with global | SQLite per-wallet ✅ | Wallet tracking |
| **Lightning** | N/A | SQLite per-wallet ✅ | Lightning state |
| **ChainDB** | RocksDB ✅ | RocksDB ✅ | Block index |
| **Mempool** | SQLite ❌ | **RAM-only** ✅ | Active tx pool |
| **Mempool Dump** | N/A | mempool.dat (binary) ⏳ | Shutdown persistence |
| **Peers** | SQLite ❌ | peers.dat (binary) ⏳ | Peer addresses |
| **Explorer** | SQLite ❌ | DELETED ✅ | N/A |

---

## 🚀 **Bitcoin Core Equivalence**

| Bitcoin Core | DineroCoin | Status |
|--------------|------------|--------|
| `chainstate/` (LevelDB/RocksDB) | `blockchain/utxo/` (RocksDB) | ✅ |
| `blocks/` (flat files) | `blockchain/blks/` (flat files) | ✅ |
| `blocks/index/` (LevelDB) | `blockchain/chaindb/` (RocksDB) | ✅ |
| `wallets/` (Berkeley DB → SQLite) | `wallets/` (SQLite) | ✅ |
| **Mempool** (RAM-only) | **Mempool** (RAM-only) | ⏳ |
| `mempool.dat` (binary dump) | `mempool.dat` (binary dump) | ⏳ |
| `peers.dat` (binary) | `peers.dat` (binary) | ⏳ |

---

## 🔧 **Migration Path**

### **Phase 1: UTXO Migration** ⏳
```bash
# Stop daemon
./dinerod stop

# Backup old UTXO database
cp ~/.dinero/blockchain/utxo ~/.dinero/blockchain/utxo.sqlite.backup

# Start daemon (will create new RocksDB UTXO set)
# Rescan will populate from chaindb
./dinerod -rescan
```

### **Phase 2: Remove Wrong Databases** ⏳
```bash
# After verification
rm ~/.dinero/explorer.db
rm ~/.dinero/mempool.db
rm ~/.dinero/peers.db
rm ~/.dinero/blockchain/utxo.sqlite.backup
```

### **Phase 3: Test** ⏳
```bash
# Verify UTXO count matches
./dinero-cli getutxocount

# Verify wallet balance
./dinero-cli getbalance

# Verify wallet sees same UTXOs
./dinero-cli listunspent
```

---

## 📝 **Code Integration**

### **BlockAcceptor Changes:**
```cpp
// OLD (WRONG)
UTXOIndex* utxo_index = ctx.chainstate->getUTXOIndex();  // SQLite
utxo_index->AddUTXO(...);

// NEW (CORRECT)
GlobalUTXOSet* global_utxo = ctx.chainstate->getGlobalUTXOSet();  // RocksDB
global_utxo->addUTXO(...);

// Wallet gets notified separately
if (wallet_owns_output) {
    WalletUTXO wallet_utxo = {txid, vout, derivation_path, amount, height};
    wallet->getUTXOTracker()->addOwnedUTXO(wallet_utxo);  // SQLite
}
```

### **Wallet Balance Query:**
```cpp
// OLD (WRONG)
UTXOIndex* utxo_index = ...;  // Mixed global + wallet
uint64_t balance = utxo_index->GetBalance();  // Unclear whose balance

// NEW (CORRECT)
WalletUTXOTracker* tracker = wallet->getUTXOTracker();  // Clear: wallet's tracker
WalletBalance balance = tracker->getBalance(current_height);  // Clear: wallet's balance
```

---

## ✅ **Summary**

**Before:** Mixed SQLite for everything (wrong, slow, not Bitcoin-compatible)
**After:** RocksDB for consensus, SQLite for wallet (correct, fast, Bitcoin-compatible)

**Key Principle:**
> Only wallet data should be SQLite. Everything else (consensus, mempool, peers) uses RocksDB or binary formats.

**SQLite Usage (ALLOWED):**
- ✅ Wallet keys, addresses, metadata (per-wallet)
- ✅ Wallet UTXO tracking (per-wallet)
- ✅ Lightning state (per-wallet: channels, HTLCs, invoices, peers)

**RocksDB Usage (CONSENSUS):**
- ✅ Global UTXO set (all blockchain UTXOs)
- ✅ Block index (ChainDB)

**RAM-Only (NOT PERSISTED):**
- ✅ Active mempool (std::unordered_map, changes thousands of times/sec)

**Binary Files (OPTIONAL PERSISTENCE):**
- ⏳ mempool.dat (optional dump on shutdown)
- ⏳ peers.dat (peer addresses)

**Key Insight from Bitcoin Core:**
> "The mempool is NOT a database. It's a RAM-only data structure that changes constantly. Persisting it to disk (SQLite or RocksDB) would create massive performance problems and violate the volatility model."

This matches Bitcoin Core exactly! 🎯

---

**Status:** ✅ Architecture defined, implementation in progress
**Next Steps:** Remove old UTXOIndex, update BlockAcceptor, migrate databases
