# Dinero Three-Layer Data Model - ARCHITECTURE CONFIRMED ✅

## ✅ **CONFIRMED: Clean Three-Layer Architecture**

Dinero implements a **professional-grade hybrid database model** used by production blockchains (Zcash, Dash, Chia).

---

## 🏗️ **Layer 1: RocksDB - Blockchain's Memory (Truth)**

**Purpose**: **Single source of truth** for blockchain state

**Implementation**: `ChainDB` class (`src/storage/chain_db.cpp`)

**Storage**:
- **Blocks**: Full block data
- **Headers**: Block headers with height/work
- **UTXO Set**: Unspent transaction outputs (canonical state)
- **Chain State**: Tip hash, height, chainwork
- **Transaction Index**: TXID → block location

**Usage**:
- `BlockAcceptor` writes all blocks/headers/UTXOs to RocksDB
- `ChainDB::putBlock()`, `putHeader()`, `putCoin()` - Write operations
- `ChainDB::getBlock()`, `getHeader()`, `getCoin()` - Read operations
- **Atomic batches** via `rocksdb::WriteBatch`

**Location**: `blockchain/chaindb/` (RocksDB directory)

**Column Families**:
- `blocks` - Full block data
- `headers` - Block headers + height + work
- `height` - Height-to-hash index
- `utxo` - UTXO set (canonical truth)
- `txindex` - Transaction index
- `meta` - Metadata (tip, schema)

**Characteristics**:
- ✅ **High performance** - Optimized for sequential writes
- ✅ **ACID transactions** - Atomic batch writes
- ✅ **Immutable** - Append-only blockchain data
- ✅ **Production-grade** - Used by Bitcoin Core, Zcash, Dash

---

## 🔐 **Layer 2: SQLite - Wallet's Memory (Private)**

**Purpose**: **Private wallet data** - keys, addresses, transaction history

**Implementation**: `WalletManager` class (`src/wallet/wallet_manager.cpp`)

**Storage**:
- **Wallet Database**: `wallet.db`
- **Keys**: Encrypted private keys
- **Addresses**: HD wallet addresses (BIP44/BIP84)
- **Transactions**: Wallet transaction history
- **UTXOs**: Wallet-owned UTXOs (subset of chain UTXO set)
- **Labels**: Address labels, account names

**Usage**:
- `WalletManager` manages all wallet operations
- SQLite transactions for atomicity
- Encrypted storage for sensitive data
- HD wallet derivation (BIP39/BIP44/BIP84)

**Location**: `wallet.db` (via `SQLiteManager`)

**Tables**:
- `wallets` - Wallet metadata
- `addresses` - HD addresses
- `transactions` - Transaction history
- `utxos` - Wallet-owned UTXOs
- `keys` - Encrypted keys (if applicable)

**Characteristics**:
- ✅ **Privacy** - Separate from chain data
- ✅ **Portability** - Easy backup/restore
- ✅ **ACID** - SQL transactions
- ✅ **Query flexibility** - SQL for complex wallet queries

---

## 📊 **Layer 3: SQLite Index - RPC's Memory (Public View)**

**Purpose**: **Fast RPC queries** - Public blockchain view for API access

**Implementation**: `Blockchain` class (`src/daemon/blockchain.cpp`)

**Storage**:
- **Blockchain Database**: `blockchain.db` (via `SQLiteManager`)
- **Block Index**: Quick block lookups by height/hash
- **Chain State**: Best block hash/height (cached)
- **UTXO Index**: Fast UTXO queries for RPC
- **Transaction Index**: TXID → block mapping

**Usage**:
- RPC methods query `Blockchain` class
- `getBlock()`, `getBlockByHash()`, `getUTXO()` - Fast queries
- `hasBlock()`, `getBlockHeight()` - Quick checks
- **Read-only** for most RPC operations

**Location**: `blockchain.db` (via `SQLiteManager`)

**Tables**:
- `block_index` - Block metadata (hash, height, version, timestamp, bits, nonce)
- `chain_state` - Current chain tip (best_block_hash, best_block_height)
- `utxo` - UTXO set (for RPC queries)
- `tx_index` - Transaction index (tx_hash → block_hash, block_height)

**Characteristics**:
- ✅ **Fast queries** - SQL indexes for O(log n) lookups
- ✅ **Public API** - No sensitive wallet data
- ✅ **Read-optimized** - Optimized for RPC queries
- ✅ **Cached view** - Mirrors RocksDB data for fast access

---

## 🔄 **Data Flow**

```
┌─────────────────────────────────────────────────────────┐
│                    BlockAcceptor                        │
│              (Block Processing Engine)                  │
└─────────────────────────────────────────────────────────┘
                        │
                        │ Writes
                        ▼
        ┌───────────────────────────────┐
        │   Layer 1: RocksDB (Truth)   │
        │         ChainDB                │
        │  - Blocks, Headers, UTXO Set  │
        │  - Single Source of Truth      │
        └───────────────────────────────┘
                        │
        ┌───────────────┴───────────────┐
        │                               │
        ▼                               ▼
┌───────────────┐            ┌──────────────────┐
│ Layer 2:      │            │ Layer 3:         │
│ SQLite Wallet │            │ SQLite Index     │
│ (Private)     │            │ (Public View)    │
│               │            │                   │
│ WalletManager │            │ Blockchain        │
│ - Keys        │            │ - Block Index     │
│ - Addresses   │            │ - Chain State     │
│ - TX History  │            │ - UTXO Index      │
│ - Labels      │            │ - TX Index        │
└───────────────┘            └──────────────────┘
        │                               │
        │                               │
        ▼                               ▼
┌───────────────┐            ┌──────────────────┐
│ Wallet RPC    │            │ Blockchain RPC   │
│ (Private)     │            │ (Public)         │
│               │            │                   │
│ - getbalance  │            │ - getblock        │
│ - listtx      │            │ - getblockhash    │
│ - sendto      │            │ - getutxo         │
│ - getaddress  │            │ - getblockcount   │
└───────────────┘            └──────────────────┘
```

---

## 🎯 **Separation of Concerns**

### **Layer 1 (RocksDB) - Truth**
- **Write-heavy** - Optimized for sequential block writes
- **Immutable** - Append-only blockchain data
- **Canonical** - Single source of truth for UTXO set
- **Performance** - High-throughput block processing

### **Layer 2 (SQLite Wallet) - Privacy**
- **Private** - Never exposed via RPC
- **Encrypted** - Sensitive key material
- **Portable** - Easy backup/restore
- **Isolated** - Separate from chain data

### **Layer 3 (SQLite Index) - Performance**
- **Read-optimized** - Fast RPC queries
- **Public** - Safe to expose via API
- **Cached** - Mirrors RocksDB for speed
- **Indexed** - SQL indexes for O(log n) lookups

---

## ✅ **Benefits of This Architecture**

### **1. Performance**
- **RocksDB**: Fast sequential writes (block processing)
- **SQLite Index**: Fast random reads (RPC queries)
- **Best of both worlds**: Write-optimized + Read-optimized

### **2. Scalability**
- **RocksDB**: Handles millions of blocks efficiently
- **SQLite Index**: Fast queries even with large chain
- **Separation**: Each layer optimized for its use case

### **3. Security**
- **Wallet isolation**: Private keys never in public database
- **Read-only RPC**: Public API can't modify chain state
- **Encryption**: Wallet data encrypted separately

### **4. Maintainability**
- **Clear separation**: Each layer has distinct purpose
- **Easy backup**: Wallet and chain data separate
- **Professional**: Matches production blockchain architecture

---

## 📋 **Comparison with Production Blockchains**

| Blockchain | Chain Storage | Wallet Storage | RPC/Index |
|------------|---------------|----------------|-----------|
| **Bitcoin Core** | LevelDB/RocksDB | BDB/SQLite | LevelDB/RocksDB |
| **Zcash** | RocksDB | SQLite | RocksDB + SQLite |
| **Dash** | LevelDB | SQLite | LevelDB + SQLite |
| **Chia** | SQLite (chain) | SQLite (wallet) | SQLite |
| **Dinero** | **RocksDB** | **SQLite** | **SQLite Index** ✅ |

---

## 🎉 **Conclusion**

✅ **CONFIRMED**: Dinero implements a **clean three-layer data model**:

1. **RocksDB** - Blockchain's memory (truth) ✅
2. **SQLite** - Wallet's memory (private) ✅
3. **SQLite Index** - RPC's memory (public view) ✅

This architecture provides:
- ✅ **Bitcoin-class performance** (RocksDB for chain)
- ✅ **Mobile-class accessibility** (SQLite for wallet/RPC)
- ✅ **Professional-grade separation** (matches Zcash/Dash/Chia)

**Status**: Production-ready hybrid database model ✅

