# DineroCoin Canonical Storage Layout

**Date:** December 4, 2025
**Status:** ✅ FINAL - This is the law. Do not deviate.

---

## 🔥 **The Rules of the Universe**

### **Rule 1: Consensus-Critical Data = RocksDB**
- Block index
- Global UTXO set
- Chainstate
- **NEVER SQLite for consensus**

### **Rule 2: Wallet/Node Data = SQLite (per wallet)**
- HD keys, addresses
- Wallet-owned UTXOs (wallet view)
- Transaction metadata, labels
- Lightning state (channels, HTLCs, invoices)
- **ALWAYS per-wallet isolation**

### **Rule 3: High-Frequency Volatile Data = RAM Only**
- Active mempool (changes thousands of times/sec)
- Lightning state caches (ChannelManager, HTLCManager)
- Network graph (gossip)
- **NEVER persist to database during operation**

### **Rule 4: Optional Convenience Dumps = Binary Files**
- mempool.dat (shutdown dump)
- peers.dat (peer addresses)
- lightning_gossip.dat (gossip graph)
- **NOT databases, just binary serialization**

### **Rule 5: One Purpose, One Storage**
- NO mixing consensus + wallet in same database
- NO mixing global + per-wallet in same table
- NO duplicate storage (explorer.db duplicating chaindb)
- **Each piece of data has EXACTLY ONE authoritative location**

---

## ✅ **Canonical Storage Layout**

```
~/.dinero/
├── blockchain/
│   ├── blks/                 ← Flat block files (like Bitcoin blkNNNNN.dat)
│   ├── chaindb/              ← RocksDB (chainstate + block index; CONSENSUS)
│   └── utxo/                 ← RocksDB (GLOBAL UTXO set; CONSENSUS)
│
├── mempool.dat               ← Binary dump (optional, from RAM mempool on shutdown)
├── peers.dat                 ← Binary peers (addresses, bans, scores)
├── lightning_gossip.dat      ← Binary dump of gossip graph (optional, non-SQL)
│
└── wallets/
    └── {wallet_name}/
        ├── wallet.db         ← SQLite, per wallet:
        │                        - HD keys, descriptors/derivation
        │                        - Wallet-owned UTXOs (wallet view)
        │                        - Transaction metadata, labels, settings
        │
        └── lightning.db      ← SQLite, per wallet's Lightning node:
                                 - channels table
                                 - HTLCs
                                 - commitments/revocation secrets
                                 - invoices/payments
                                 - watchtower metadata

MEMORY ONLY (NOT PERSISTED):
- mempool                     ← std::unordered_map / maps / heaps
- Lightning state caches:
  - ChannelManager
  - HTLCManager
  - Gossip/NetworkGraph
  - PaymentRouter
```

---

## 🗄️ **Storage Responsibility Matrix**

| Data Type | Storage | Location | Purpose |
|-----------|---------|----------|---------|
| **Block headers** | RocksDB | `blockchain/chaindb/` | Consensus |
| **Block data** | Flat files | `blockchain/blks/` | Consensus |
| **Global UTXO set** | RocksDB | `blockchain/utxo/` | Consensus |
| **Wallet keys** | SQLite | `wallets/{name}/wallet.db` | Per-wallet |
| **Wallet UTXOs** | SQLite | `wallets/{name}/wallet.db` | Per-wallet |
| **Lightning channels** | SQLite | `wallets/{name}/lightning.db` | Per-wallet |
| **Lightning HTLCs** | SQLite | `wallets/{name}/lightning.db` | Per-wallet |
| **Lightning invoices** | SQLite | `wallets/{name}/lightning.db` | Per-wallet |
| **Lightning payments** | SQLite | `wallets/{name}/lightning.db` | Per-wallet |
| **Active mempool** | RAM | Memory (std::unordered_map) | Volatile |
| **Mempool dump** | Binary | `mempool.dat` | Optional |
| **Peer addresses** | Binary | `peers.dat` | Optional |
| **Lightning gossip** | Binary | `lightning_gossip.dat` | Optional |

---

## 🧠 **Why Lightning Uses SQLite (Per Wallet)**

Lightning state is:

1. **Not consensus-critical**
   - Two nodes can disagree on channel state (that's the point!)
   - Breaches + watchtowers exist because of this
   - Each node has its own view

2. **Node-specific**
   - Per wallet/node identity
   - Node pubkey tied to wallet
   - Channels belong to THIS wallet

3. **Rich & relational**
   - Channels, HTLCs, payments, invoices
   - Forwards, penalties, revocations
   - Complex queries needed

4. **SQLite is perfect for this:**
   - ✅ Fast enough (everything is local)
   - ✅ Easy to migrate with ALTER TABLE
   - ✅ Transactional (no half-written state)
   - ✅ Embeddable, single file per wallet

**Rule:** Consensus-critical = RocksDB, Node/wallet/Lightning = SQLite

---

## ⚠️ **What We're Fixing (The Danger Zone)**

### **Current Problems:**

1. ❌ **Lightning tables split between wallet.db and lightning.db**
   - Some in wallet.db (ln_* tables)
   - Some might be in separate lightning.db
   - **Risk:** Inconsistent state, data corruption

2. ❌ **Legacy junk databases:**
   - explorer.db (duplicates chaindb)
   - mempool.db (should be RAM)
   - peers.db (should be binary)
   - **Risk:** Confusion, wasted storage, incorrect queries

3. ❌ **Old utxo SQLite pretending to be global consensus**
   - `~/.dinero/blockchain/utxo` as SQLite file
   - Should be RocksDB directory
   - **Risk:** Consensus bugs, performance problems

4. ❌ **UTXOIndex mixing responsibilities:**
   - Half global UTXO index
   - Half wallet UTXO tracker
   - **Risk:** Unclear ownership, data duplication

5. ❌ **Conflicting schema files:**
   - Multiple wallet_schema.sql versions
   - Schema files for removed databases
   - **Risk:** Applying wrong schema, corruption

---

## ✅ **The Fix: One Purpose, One Location**

### **Consensus Data (RocksDB):**

**File:** `src/consensus/global_utxo_set.cpp`
**Storage:** `~/.dinero/blockchain/utxo/` (RocksDB directory)

```cpp
class GlobalUTXOSet {
    // ALL blockchain UTXOs (consensus-critical)
    // NO wallet-specific data
    // NO derivation paths, labels, locks
};
```

---

### **Wallet Data (SQLite):**

**File:** `wallets/{wallet_name}/wallet.db`
**Schema:** `resources/schema/wallet_schema.sql`

```sql
-- HD Wallet
CREATE TABLE wallet_meta (...);
CREATE TABLE hd_seeds (...);
CREATE TABLE addresses (...);
CREATE TABLE address_derivation_paths (...);

-- Wallet UTXOs (wallet view of global set)
CREATE TABLE wallet_utxos (
    txid TEXT NOT NULL,
    vout INTEGER NOT NULL,
    derivation_path TEXT NOT NULL,      -- "m/84'/1447'/0'/0/5"
    cached_amount INTEGER NOT NULL,     -- Cached from GlobalUTXOSet
    cached_height INTEGER NOT NULL,     -- Cached from GlobalUTXOSet
    is_spent INTEGER NOT NULL DEFAULT 0,
    is_locked INTEGER NOT NULL DEFAULT 0,  -- Coin control
    label TEXT,
    PRIMARY KEY (txid, vout)
);

-- Transaction history
CREATE TABLE transactions (...);
CREATE TABLE tx_io (...);
```

---

### **Lightning Data (SQLite, Per Wallet):**

**File:** `wallets/{wallet_name}/lightning.db`
**Schema:** `resources/schema/lightning_schema.sql`

```sql
-- Lightning Channels
CREATE TABLE ln_channels (
    channel_id TEXT PRIMARY KEY,
    peer_node_id TEXT NOT NULL,
    funding_txid TEXT NOT NULL,
    funding_vout INTEGER NOT NULL,
    funding_amount_una INTEGER NOT NULL,
    local_balance_muna INTEGER NOT NULL,
    remote_balance_muna INTEGER NOT NULL,
    state INTEGER NOT NULL DEFAULT 0,
    commitment_number INTEGER NOT NULL DEFAULT 0,
    -- ... all channel state
);

-- Lightning HTLCs
CREATE TABLE ln_htlcs (
    htlc_id TEXT PRIMARY KEY,
    channel_id TEXT NOT NULL,
    amount_muna INTEGER NOT NULL,
    payment_hash TEXT NOT NULL,
    cltv_expiry INTEGER NOT NULL,
    is_incoming INTEGER NOT NULL DEFAULT 0,
    state INTEGER NOT NULL DEFAULT 0,
    -- ... routing info
);

-- Lightning Commitments
CREATE TABLE ln_commitments (
    commitment_id TEXT PRIMARY KEY,
    channel_id TEXT NOT NULL,
    commitment_num INTEGER NOT NULL,
    local_sig TEXT,
    remote_sig TEXT,
    tx_data TEXT
);

-- Lightning Peers
CREATE TABLE ln_peers (
    node_id TEXT PRIMARY KEY,
    address TEXT,
    last_seen INTEGER NOT NULL DEFAULT 0,
    trusted INTEGER NOT NULL DEFAULT 0
);

-- Lightning Invoices (BOLT #11)
CREATE TABLE ln_invoices (
    payment_hash TEXT PRIMARY KEY,
    bolt11_string TEXT NOT NULL,
    preimage TEXT,
    amount_muna INTEGER NOT NULL DEFAULT 0,
    description TEXT,
    status INTEGER NOT NULL DEFAULT 0,
    -- ... invoice metadata
);

-- Lightning Payments
CREATE TABLE ln_payments (
    payment_hash TEXT PRIMARY KEY,
    bolt11_string TEXT,
    destination_node_id TEXT NOT NULL,
    amount_muna INTEGER NOT NULL,
    fee_muna INTEGER NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,
    preimage TEXT,
    -- ... route info
);

-- Lightning Revocation Secrets (encrypted)
CREATE TABLE ln_secrets (
    channel_id TEXT NOT NULL,
    commitment_num INTEGER NOT NULL,
    secret_data BLOB NOT NULL,
    PRIMARY KEY (channel_id, commitment_num)
);

-- Watchtower Metadata
CREATE TABLE ln_watchtower (
    channel_id TEXT NOT NULL,
    breach_txid TEXT,
    penalty_tx BLOB,
    -- ... watchtower data
);
```

**Why separate lightning.db:**
- ✅ Clear separation: on-chain (wallet.db) vs off-chain (lightning.db)
- ✅ Independent backups (can backup wallet.db without Lightning)
- ✅ Easier to implement wallet without Lightning
- ✅ Lightning can be disabled per wallet

---

### **Mempool (RAM Only):**

**File:** `src/daemon/mempool_manager.cpp`
**Storage:** Memory (std::unordered_map)

```cpp
class MempoolManager {
private:
    // RAM ONLY - NO DATABASE
    std::unordered_map<uint256, MempoolEntry> m_transactions;
    std::multimap<double, uint256> m_fee_rate_index;
    std::unordered_map<uint256, std::set<uint256>> m_ancestors;
    std::unordered_map<uint256, std::set<uint256>> m_descendants;

public:
    // Optional: Save to binary file on shutdown
    bool dumpMempool(const std::string& filepath);  // → mempool.dat
    bool loadMempool(const std::string& filepath);  // ← mempool.dat
};
```

**Why RAM-only:**
- ✅ Thousands of changes per second
- ✅ Non-consensus (each node has different mempool)
- ✅ Disk I/O would create bottlenecks
- ✅ Bitcoin Core does it this way

---

## 🚫 **What NOT to Do (Violations)**

### ❌ **DON'T: Create explorer.db**
```bash
# WRONG
~/.dinero/explorer.db  # Duplicates chaindb, breaks single-source-of-truth
```

**Why wrong:** Block index already in chaindb (RocksDB), querying should go directly there.

---

### ❌ **DON'T: Create mempool.db**
```bash
# WRONG
~/.dinero/mempool.db  # Mempool must be RAM-only
```

**Why wrong:** Mempool changes thousands of times per second, disk I/O kills performance.

---

### ❌ **DON'T: Create peers.db**
```bash
# WRONG
~/.dinero/peers.db  # Peers should be binary format
```

**Why wrong:** Peer addresses are simple structs, don't need SQL overhead.

---

### ❌ **DON'T: Store Lightning in wallet.db**
```sql
-- WRONG (mixing on-chain + off-chain)
wallet.db:
  - wallet tables
  - ln_* tables  ← WRONG! Put in lightning.db
```

**Why wrong:** Lightning state should be separate from on-chain wallet state.

**Correct:**
```
wallet.db      ← On-chain wallet data
lightning.db   ← Off-chain Lightning data
```

---

### ❌ **DON'T: Mix global + wallet UTXO tracking**
```cpp
// WRONG
class UTXOIndex {
    // Stores ALL blockchain UTXOs
    // + Stores wallet-specific metadata
    // = Confused responsibility
};
```

**Why wrong:** Consensus data (global UTXOs) mixed with application data (wallet ownership).

**Correct:**
```cpp
// Consensus (RocksDB)
class GlobalUTXOSet {
    // ALL blockchain UTXOs
    // NO wallet-specific data
};

// Application (SQLite per wallet)
class WalletUTXOTracker {
    // THIS wallet's UTXOs only
    // Wallet-specific metadata (derivation path, labels)
};
```

---

### ❌ **DON'T: Have multiple schema versions**
```bash
# WRONG
database/schema/wallet_schema.sql       ← Old version (120 lines)
resources/schema/wallet_schema.sql      ← New version (371 lines)
```

**Why wrong:** Code applies wrong schema, creates inconsistent databases.

**Correct:**
```bash
# Only one schema per component
resources/schema/wallet_schema.sql      ← ONLY wallet schema
resources/schema/lightning_schema.sql   ← ONLY Lightning schema
```

---

## 📋 **Schema File Organization**

### **Correct:**
```
resources/schema/
├── wallet_schema.sql       ← On-chain wallet schema (HD keys, UTXOs, txs)
└── lightning_schema.sql    ← Off-chain Lightning schema (channels, HTLCs, invoices)
```

### **NO other schema files:**
- ❌ NO explorer_schema.sql
- ❌ NO mempool_schema.sql
- ❌ NO peers_schema.sql
- ❌ NO blockchain_schema.sql

**Why:** Those components don't use SQL.

---

## ✅ **Migration Checklist**

### **Phase 1: Clean up schema files**
- [ ] Delete `database/schema/` directory entirely
- [ ] Keep only `resources/schema/wallet_schema.sql`
- [ ] Create `resources/schema/lightning_schema.sql`
- [ ] Move Lightning tables from wallet_schema.sql → lightning_schema.sql

### **Phase 2: Update wallet schema**
- [ ] Add `wallet_utxos` table to wallet_schema.sql
- [ ] Mark legacy `utxos` table as deprecated
- [ ] Remove Lightning tables from wallet_schema.sql

### **Phase 3: Update code**
- [ ] Remove `getBlockchainDB()`, `getMempoolDB()`, `getPeersDB()` from SQLiteManager
- [ ] Delete `blockchain.cpp` (legacy SQLite wrapper)
- [ ] Update `explorer_db_service.cpp` to query ChainDB directly
- [ ] Update BlockAcceptor to use GlobalUTXOSet

### **Phase 4: Separate Lightning**
- [ ] Create SQLiteLightningDB that opens `lightning.db` (not wallet.db)
- [ ] Move Lightning table creation to lightning_schema.sql
- [ ] Update LightningService to use separate database

### **Phase 5: Verify**
- [ ] No explorer.db created
- [ ] No mempool.db created
- [ ] No peers.db created
- [ ] wallet.db has only on-chain data
- [ ] lightning.db has only off-chain data
- [ ] Mempool is RAM-only

---

## 🎯 **Success Criteria**

When complete:

1. ✅ `blockchain/chaindb/` (RocksDB) - block index
2. ✅ `blockchain/utxo/` (RocksDB) - global UTXO set
3. ✅ `wallets/{name}/wallet.db` (SQLite) - on-chain wallet
4. ✅ `wallets/{name}/lightning.db` (SQLite) - off-chain Lightning
5. ✅ Active mempool in RAM (no database)
6. ✅ Optional mempool.dat binary dump
7. ✅ Optional peers.dat binary format
8. ✅ NO explorer.db, mempool.db, peers.db
9. ✅ NO Lightning tables in wallet.db
10. ✅ ONE schema file per component

---

## 🔥 **The Law**

> **Consensus = RocksDB**
> **Wallet/Lightning = SQLite (per wallet)**
> **Mempool = RAM**
> **One purpose, one location, zero overlap**

**Do not deviate from this architecture.**

---

**Status:** ✅ CANONICAL - This is the final answer.
