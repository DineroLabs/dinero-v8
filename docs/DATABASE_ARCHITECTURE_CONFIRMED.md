# Database Architecture Confirmation

## ✅ CONFIRMED: Dual Database Architecture

### **SQLite for Wallet** ✅
**Location**: `src/wallet/wallet_manager.cpp`, `include/dinero/core/database/sqlite_manager.h`

**Usage**:
- `WalletManager` uses SQLite for all wallet operations
- `SQLiteManager` provides `getWalletDB()` - SQLite database
- Stores: wallet data, addresses, transactions, UTXOs, keys

**Database File**: `wallet.db` (via SQLiteManager)

---

### **RocksDB for Chain** ✅
**Location**: `src/storage/chain_db.cpp`, `include/storage/chain_db.h`

**Usage**:
- `ChainDB` uses RocksDB for all chain operations
- Used by `BlockAcceptor` for block/header/UTXO storage
- Stores: blocks, headers, UTXO set, chain state, transaction index

**Database Directory**: `blockchain/chaindb/` (RocksDB directory)

**Column Families**:
- `blocks` - Full block data
- `headers` - Block headers
- `height` - Height-to-hash index
- `utxo` - UTXO set
- `txindex` - Transaction index
- `meta` - Metadata (tip, schema version)

---

## ⚠️ **Important Note: Dual Storage System**

The codebase has **TWO separate storage systems**:

### 1. **Blockchain Class (SQLite)** - Legacy/Compatibility Layer
**File**: `src/daemon/blockchain.cpp`
- Uses `SQLiteManager` → `getBlockchainDB()` → SQLite
- Stores: `block_index`, `chain_state`, `utxo`, `tx_index` tables
- **Purpose**: RPC compatibility, legacy queries
- **Status**: Still used for some RPC methods

### 2. **ChainDB Class (RocksDB)** - Production Chain Storage
**File**: `src/storage/chain_db.cpp`
- Uses RocksDB directly
- Stores: blocks, headers, UTXO set, chain state
- **Purpose**: Primary chain storage, used by `BlockAcceptor`
- **Status**: Active production storage

---

## **Current Architecture**

```
┌─────────────────────────────────────────┐
│         ChainstateService                │
├─────────────────────────────────────────┤
│  ┌──────────────┐    ┌──────────────┐   │
│  │  Blockchain  │    │   ChainDB    │   │
│  │   (SQLite)   │    │  (RocksDB)   │   │
│  │              │    │              │   │
│  │ - RPC compat │    │ - Block ops  │   │
│  │ - Legacy     │    │ - Header ops │   │
│  │ - Queries    │    │ - UTXO ops   │   │
│  └──────────────┘    └──────────────┘   │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│         WalletService                   │
├─────────────────────────────────────────┤
│  ┌──────────────┐                       │
│  │ WalletManager│                       │
│  │   (SQLite)   │                       │
│  │              │                       │
│  │ - Wallet DB  │                       │
│  │ - Keys       │                       │
│  │ - Addresses  │                       │
│  └──────────────┘                       │
└─────────────────────────────────────────┘
```

---

## **Summary**

| Component | Database | Purpose | Status |
|-----------|----------|---------|--------|
| **Wallet** | SQLite | Wallet data, keys, addresses | ✅ Active |
| **Chain (Primary)** | RocksDB | Blocks, headers, UTXO set | ✅ Active |
| **Blockchain (Legacy)** | SQLite | RPC compatibility, queries | ⚠️ Dual storage |

**Answer**: ✅ **YES** - SQLite for wallet, RocksDB for chain (via ChainDB)

**Note**: The `Blockchain` class also uses SQLite, but this appears to be a legacy/compatibility layer. The primary chain storage is RocksDB via `ChainDB`.

