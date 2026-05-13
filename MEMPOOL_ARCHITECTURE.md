# Mempool Architecture - Bitcoin Core Model

**Date:** December 4, 2025
**Status:** Clarified - RAM-only model

---

## 🔥 **Core Principle: Mempool is RAM-Only**

**DineroCoin must follow Bitcoin Core's approach:**
- ✅ Active mempool lives entirely in **RAM** (no database)
- ✅ Optional `mempool.dat` binary dump on shutdown (convenience only)
- ❌ NO SQLite for mempool
- ❌ NO RocksDB for mempool
- ❌ NO LevelDB for mempool

---

## 🧠 **Why RAM-Only?**

### **1. Extremely High Write/Read Frequency**

Mempool changes constantly:
- New transactions arriving (every second)
- Block confirmations (remove all confirmed txs)
- RBF (Replace-By-Fee) replacements
- Transaction evictions (when mempool is full)
- Fee-rate recalculations
- Ancestor/descendant chain updates
- CPFP (Child-Pays-For-Parent) handling

**Any disk-based database (SQLite, RocksDB) would:**
- ❌ Create massive lock contention
- ❌ Write amplification (every tx = multiple disk writes)
- ❌ I/O bottleneck (disk can't keep up)
- ❌ Latency spikes
- ❌ Performance degradation

---

### **2. Mempool Contents Are Non-Consensus**

The mempool is:
- **Optional** - nodes can have different mempools
- **Volatile** - can be cleared at any time
- **Local** - not shared with other nodes
- **Non-deterministic** - peers may have completely different sets

**Why this matters:**
- No need for persistence
- No need for durability guarantees
- No need for ACID properties
- Can be completely empty on restart (this is fine!)

---

### **3. Persistence Adds More Problems Than It Solves**

If you store mempool on disk:
- ❌ May reload stale transactions after restart
- ❌ May contain conflicts with confirmed blocks
- ❌ May violate new policy rules after upgrade
- ❌ Waste storage on irrelevant transactions
- ❌ Slow startup (loading large mempool.db)

Bitcoin Core chose **RAM-only** for a reason.

---

## 🧊 **Bitcoin Core's Optional Persistence**

Bitcoin Core does write `mempool.dat`, but:

**What it is:**
- 🟦 Single binary dump file
- 🟦 Written on daemon shutdown
- 🟦 Loaded on daemon startup
- 🟦 NOT a database (no queries, no transactions)
- 🟦 Convenience feature (NOT correctness)

**When it's invalidated:**
- Protocol upgrades
- Policy changes
- Consensus rule changes
- Corrupted file

**Purpose:**
- Convenience only (faster restart)
- NOT required for correctness
- Mempool can be empty on restart

---

## ✅ **DineroCoin Implementation**

### **Active Mempool (RAM Only)**

```cpp
// src/daemon/mempool_manager.h
class MempoolManager {
public:
    // Add transaction to mempool
    bool addTransaction(const Transaction& tx);

    // Remove transaction (e.g., after confirmation)
    bool removeTransaction(const uint256& txid);

    // Get transaction by hash
    std::optional<Transaction> getTransaction(const uint256& txid);

    // Get transactions for mining (sorted by fee-rate)
    std::vector<Transaction> getTransactionsForBlock(uint64_t max_weight);

    // Clear all transactions
    void clear();

    // Get mempool size
    size_t size() const;
    uint64_t getTotalFees() const;

private:
    // PRIMARY DATA STRUCTURE (RAM ONLY)
    std::unordered_map<uint256, MempoolEntry> m_transactions;

    // Fee-rate index (for mining)
    std::multimap<double, uint256> m_fee_rate_index;

    // Ancestor/descendant tracking
    std::unordered_map<uint256, std::set<uint256>> m_ancestors;
    std::unordered_map<uint256, std::set<uint256>> m_descendants;

    // RBF tracking
    std::unordered_map<uint256, RBFState> m_rbf_state;

    // Thread safety
    mutable std::mutex m_mutex;
};

struct MempoolEntry {
    Transaction tx;
    uint64_t fee;
    double fee_rate;  // una per byte
    uint64_t time_received;
    uint64_t size;
    uint64_t weight;

    // Ancestor/descendant info
    size_t ancestor_count;
    uint64_t ancestor_size;
    uint64_t ancestor_fees;
};
```

---

### **Optional Persistence (mempool.dat)**

```cpp
// src/daemon/mempool_manager.cpp

bool MempoolManager::dumpMempool(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ofstream file(filepath, std::ios::binary);
    if (!file) return false;

    // Write version
    uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Write transaction count
    uint64_t count = m_transactions.size();
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // Write each transaction
    for (const auto& [txid, entry] : m_transactions) {
        // Serialize entry (transaction + metadata)
        std::vector<uint8_t> serialized = serializeEntry(entry);
        uint64_t size = serialized.size();
        file.write(reinterpret_cast<const char*>(&size), sizeof(size));
        file.write(reinterpret_cast<const char*>(serialized.data()), size);
    }

    dinero::g_logger.info("Dumped {} transactions to {}", count, filepath);
    return true;
}

bool MempoolManager::loadMempool(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return false;

    // Read version
    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) {
        dinero::g_logger.warning("Mempool version mismatch, ignoring mempool.dat");
        return false;
    }

    // Read transaction count
    uint64_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    // Read each transaction
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t size;
        file.read(reinterpret_cast<char*>(&size), sizeof(size));

        std::vector<uint8_t> serialized(size);
        file.read(reinterpret_cast<char*>(serialized.data()), size);

        MempoolEntry entry = deserializeEntry(serialized);

        // Re-validate transaction (policy may have changed)
        if (validateTransaction(entry.tx)) {
            m_transactions[entry.tx.getHash()] = entry;
        }
    }

    dinero::g_logger.info("Loaded {} transactions from {}", m_transactions.size(), filepath);
    return true;
}
```

---

### **Daemon Lifecycle**

```cpp
// src/daemon/daemon.cpp

void Daemon::start() {
    // Initialize mempool (RAM only)
    m_mempool_mgr = std::make_unique<MempoolManager>();

    // Optionally load mempool.dat
    std::string mempool_path = m_datadir + "/mempool.dat";
    if (std::filesystem::exists(mempool_path)) {
        dinero::g_logger.info("Loading mempool from {}", mempool_path);
        m_mempool_mgr->loadMempool(mempool_path);
    } else {
        dinero::g_logger.info("Starting with empty mempool (no mempool.dat found)");
    }

    // Start P2P, mining, etc.
    // ...
}

void Daemon::stop() {
    // Optionally save mempool.dat
    std::string mempool_path = m_datadir + "/mempool.dat";
    dinero::g_logger.info("Saving mempool to {}", mempool_path);
    m_mempool_mgr->dumpMempool(mempool_path);

    // Shutdown
    m_mempool_mgr.reset();
}
```

---

## 📊 **Storage Layout**

### **Correct Layout:**
```
~/.dinero/
├── blockchain/
│   ├── chaindb/              ← RocksDB (block index)
│   ├── utxo/                 ← RocksDB (global UTXO set)
│   └── blks/                 ← Flat files (raw blocks)
│
├── mempool.dat               ← Binary dump (optional, convenience only)
│
├── peers.dat                 ← Binary (peer addresses)
│
└── wallets/
    └── {wallet_name}/
        └── wallet.db         ← SQLite (per-wallet)
            ├── wallet_meta       ← Wallet metadata
            ├── hd_seeds          ← HD wallet seed
            ├── addresses         ← HD addresses
            ├── utxos             ← Wallet UTXOs
            ├── transactions      ← Tx history
            └── ln_* tables       ← Lightning (channels, HTLCs, invoices, peers, payments, secrets)

MEMORY (NOT PERSISTED):
└── mempool                   ← std::unordered_map<uint256, MempoolEntry>
                                  Active mempool lives here
```

### **What NOT to do:**
```
❌ ~/.dinero/mempool.db       (SQLite - WRONG!)
❌ ~/.dinero/mempool/         (RocksDB - WRONG!)
❌ Any database for active mempool
```

---

## 🎯 **Bitcoin Core Equivalence**

| Bitcoin Core | DineroCoin |
|--------------|------------|
| `CTxMemPool` (RAM-only) | `MempoolManager` (RAM-only) |
| `mempool.dat` (binary dump) | `mempool.dat` (binary dump) |
| NO database | NO database |

---

## ✅ **Summary**

**Active Mempool:**
- ✅ RAM-only (std::unordered_map)
- ✅ No database (no SQLite, no RocksDB)
- ✅ Changes thousands of times per second
- ✅ Non-consensus, volatile data

**Optional Persistence:**
- ✅ mempool.dat (binary dump on shutdown)
- ✅ Convenience feature (NOT correctness)
- ✅ Can be invalidated/deleted at any time

**Key Principle:**
> "The mempool is NOT a database. It's a RAM-only data structure that must be extremely fast. Bitcoin Core proved this is the only correct approach."

---

**Next Steps:**
1. Delete `database/schema/mempool_schema.sql`
2. Remove mempool.db references from `sqlite_manager.cpp`
3. Implement `MempoolManager` (RAM-only)
4. Add optional `mempool.dat` dump/load
