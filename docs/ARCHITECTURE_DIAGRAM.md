# DineroCoin Architecture Diagrams

## Layer Dependency Graph

```
┌─────────────────────────────────────────────────────────────┐
│                      GUI Layer (Qt6)                        │
│                      dinero-qt                              │
└────────────────────────────┬────────────────────────────────┘
                             │
                             │ RPC Client
                             ↓
┌─────────────────────────────────────────────────────────────┐
│                    Daemon Layer                             │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │ RPC Server   │  │ P2P Manager  │  │ Mining Manager  │  │
│  │ (HTTP + WS)  │  │ (Networking) │  │ (Coordination)  │  │
│  └──────────────┘  └──────────────┘  └─────────────────┘  │
│                         dinerod                             │
└────┬──────────────────┬──────────────────┬─────────────────┘
     │                  │                  │
     │                  │                  │
     ↓                  ↓                  ↓
┌─────────────┐  ┌──────────────┐  ┌──────────────────────┐
│   Wallet    │  │  Consensus   │  │      Storage         │
│   Layer     │  │    Layer     │  │      Layer           │
│             │  │              │  │                      │
│ • HDWallet  │  │ • Validation │  │ • ChainDB (RocksDB) │
│ • PSBT      │  │ • Difficulty │  │ • UTXOIndex         │
│ • Balance   │←─┼─• Maturity   │  │ • ChainHeight...    │
│ • TxCreate  │  │ • Subsidy    │  │   Provider          │
│             │  │              │  │                      │
└─────────────┘  └──────────────┘  └──────────────────────┘
      ↑                 ↑                      ↑
      │                 │                      │
      └─────────────────┴──────────────────────┘
                        │
                        ↓
              ┌─────────────────────┐
              │   Crypto Layer      │
              │                     │
              │ • secp256k1 (ECDSA) │
              │ • SHA256 / RIPEMD   │
              │ • Bech32 encoding   │
              │ • BIP32/39 derive   │
              └─────────────────────┘
```

## Dependency Injection: ChainHeightProvider

### Problem: Circular Dependency

```
Without DI (BAD):

Wallet ────────────────┐
  │                    │
  │  needs height      │
  │                    │
  └──────→ ChainDB ────┘
               │
               ├─ RocksDB headers
               ├─ Namespace pollution
               └─ Build coupling
```

### Solution: Interface Abstraction

```
With DI (GOOD):

┌────────────────────────────────────────────────────┐
│                  Interface Layer                   │
│  ┌──────────────────────────────────────────────┐ │
│  │     ChainHeightProvider (pure virtual)       │ │
│  │  • GetBestHeight() → uint32_t                │ │
│  │  • IsAvailable() → bool                      │ │
│  └──────────────────────────────────────────────┘ │
└───────────────┬──────────────────┬─────────────────┘
                │                  │
                │                  │
        ┌───────↓────────┐  ┌──────↓──────────────┐
        │  Wallet Uses   │  │  Storage Implements │
        │                │  │                     │
        │  HDWallet      │  │  ChainDBHeight...   │
        │    ↓           │  │       ↓             │
        │  GetBalance()  │  │  getTip().height    │
        │    checks      │  │       ↓             │
        │    maturity    │  │   RocksDB           │
        └────────────────┘  └─────────────────────┘

✅ No circular dependency
✅ No RocksDB headers in wallet
✅ Testable (mock provider)
```

## Coinbase Maturity Flow

```
┌─────────────────────────────────────────────────────────┐
│  Miner Receives Reward (Coinbase Transaction)           │
└────────────────────────┬────────────────────────────────┘
                         │
                         │ Block confirmed
                         ↓
┌─────────────────────────────────────────────────────────┐
│  Block Height: N                                        │
│  Coinbase UTXO created                                  │
│  Status: IMMATURE                                       │
└────────────────────────┬────────────────────────────────┘
                         │
                         │ Blocks 1-99 pass
                         ↓
┌─────────────────────────────────────────────────────────┐
│  Current Height: N + 99                                 │
│  Confirmations: 99                                      │
│  Status: IMMATURE (need 100)                            │
└────────────────────────┬────────────────────────────────┘
                         │
                         │ Block 100 confirmed
                         ↓
┌─────────────────────────────────────────────────────────┐
│  Current Height: N + 100                                │
│  Confirmations: 100                                     │
│  Status: MATURE ✅ Spendable!                           │
└─────────────────────────────────────────────────────────┘
                         │
                         ↓
           ┌─────────────────────────────┐
           │  Wallet Balance Display:    │
           │                             │
           │  Confirmed:  100.00 DIN ✅  │
           │  Immature:     0.00 DIN     │
           │  Total:      100.00 DIN     │
           └─────────────────────────────┘
```

## CMake Build Graph

```
dinerod (executable)
│
├─ dinero_wallet
│   ├─ dinero_crypto (PUBLIC)
│   ├─ dinero_consensus (PUBLIC)
│   └─ NO ROCKSDB ✅
│
├─ dinero_consensus
│   ├─ dinero_crypto (PUBLIC)
│   ├─ jsoncpp_static (PUBLIC)
│   ├─ secp256k1 (PUBLIC)
│   ├─ sqlite3 (PUBLIC)
│   └─ rocksdb (PRIVATE) ← Headers NOT exported!
│
├─ dinero_rpc_handlers
│   ├─ dinero_wallet (PUBLIC)
│   ├─ dinero_consensus (PUBLIC)
│   └─ rocksdb includes (PRIVATE) ← Only for headers, not linkage
│
└─ boost_system_vendored (PRIVATE)

Legend:
  PUBLIC  = Headers visible to dependents
  PRIVATE = Headers only visible internally
  ✅ = Clean isolation achieved
```

## RPC Architecture

### Modern Registry Pattern

```
┌─────────────────────────────────────────────────┐
│          RPC Client (dinero-cli / GUI)          │
└────────────────────┬────────────────────────────┘
                     │
                     │ JSON-RPC 2.0 / WebSocket
                     ↓
┌─────────────────────────────────────────────────┐
│               HTTP RPC Server                   │
│  ┌───────────────────────────────────────────┐ │
│  │        RPC Registry (Modular)             │ │
│  │  ┌─────────────────────────────────────┐ │ │
│  │  │  Blockchain Handlers                │ │ │
│  │  │  • getblockcount                    │ │ │
│  │  │  • getblock                         │ │ │
│  │  │  • getbestblockhash                 │ │ │
│  │  └─────────────────────────────────────┘ │ │
│  │  ┌─────────────────────────────────────┐ │ │
│  │  │  Wallet Handlers                    │ │ │
│  │  │  • getbalance                       │ │ │
│  │  │  • getnewaddress                    │ │ │
│  │  │  • sendtoaddress                    │ │ │
│  │  └─────────────────────────────────────┘ │ │
│  │  ┌─────────────────────────────────────┐ │ │
│  │  │  Mining Handlers                    │ │ │
│  │  │  • getmininginfo                    │ │ │
│  │  │  • generatetoaddress                │ │ │
│  │  └─────────────────────────────────────┘ │ │
│  └───────────────────────────────────────────┘ │
└─────────────────────────────────────────────────┘

✅ Replaces legacy HttpRpcServer
✅ Modular handler registration
✅ Easy to extend
```

## Data Flow: getbalance RPC

```
1. User Request
   dinero-cli getbalance
         │
         ↓
2. RPC Transport (HTTP/JSON)
   {"method":"getbalance","params":[]}
         │
         ↓
3. RPC Registry Dispatch
   registry.dispatch("getbalance")
         │
         ↓
4. Wallet Handler
   wallet->GetBalance()
         │
         ├──→ Get all UTXOs from UTXOIndex
         │
         ├──→ Get current height from ChainHeightProvider
         │              │
         │              └──→ ChainDB->getTip().height
         │
         ├──→ For each UTXO:
         │      if (is_coinbase) {
         │        if (mature) → confirmed
         │        else → immature
         │      }
         │
         └──→ Return { confirmed, immature, total }
               │
               ↓
5. JSON Response
   {
     "confirmed": "100.00000000",
     "immature":  "0.00000000",
     "total":     "100.00000000"
   }
         │
         ↓
6. User Output
   Confirmed:  100.00 DIN
   Immature:     0.00 DIN
   ─────────────────────
   Total:      100.00 DIN
```

## P2P Network Topology

```
                    Internet
                       │
         ┌─────────────┼─────────────┐
         │             │             │
         ↓             ↓             ↓
    ┌─────────┐  ┌─────────┐  ┌─────────┐
    │ Seed 1  │  │ Seed 2  │  │ Seed 3  │
    │ DNS/IP  │  │ DNS/IP  │  │ DNS/IP  │
    └────┬────┘  └────┬────┘  └────┬────┘
         │            │            │
         └────────────┼────────────┘
                      │
           P2P Message Exchange
           (tx, block, inv, getdata)
                      │
         ┌────────────┼────────────┐
         │            │            │
         ↓            ↓            ↓
    ┌─────────┐  ┌─────────┐  ┌─────────┐
    │ Peer A  │  │ Peer B  │  │ Peer C  │
    │ (Full)  │  │ (Full)  │  │ (SPV)   │
    └─────────┘  └─────────┘  └─────────┘
         │
         │ Connects to
         ↓
    ┌─────────────────────┐
    │   Local Node        │
    │   (dinerod)         │
    │                     │
    │  ┌───────────────┐ │
    │  │ P2P Manager   │ │
    │  │ • Peer DB     │ │
    │  │ • Inv relay   │ │
    │  │ • Block sync  │ │
    │  └───────────────┘ │
    └─────────────────────┘
```

## File Organization

```
DineroCoin/
│
├── src/
│   ├── daemon/          # dinerod entry point, RPC, P2P
│   │   ├── main.cpp
│   │   ├── rpc_server.cpp
│   │   └── p2p_manager.cpp
│   │
│   ├── wallet/          # HD wallet, keys, PSBT
│   │   ├── hd_wallet.cpp
│   │   ├── psbt.cpp
│   │   └── transaction.cpp
│   │
│   ├── consensus/       # Validation, difficulty, rules
│   │   ├── block_acceptor.cpp
│   │   ├── coinbase_maturity.h  ✅ Namespace fixed!
│   │   └── asert.cpp
│   │
│   ├── storage/         # ChainDB, UTXOIndex
│   │   ├── chain_db.cpp           (RocksDB isolated here)
│   │   └── chain_height_provider.cpp  ✅ DI implementation
│   │
│   ├── crypto/          # Primitives
│   │   ├── sha256.cpp
│   │   ├── bip32.cpp
│   │   └── secp256k1_wrapper.cpp
│   │
│   └── rpc/             # RPC handlers
│       ├── wallet_handlers.cpp
│       └── blockchain_handlers.cpp
│
├── include/             # Public headers
│   ├── wallet/
│   │   └── hd_wallet.h
│   ├── consensus/
│   │   └── coinbase_maturity.h
│   └── storage/
│       └── chain_height_provider.h  ✅ Clean interface
│
├── third_party/         # Vendored dependencies
│   ├── rocksdb-9.1.1/   (PRIVATE to consensus)
│   ├── boost_1_85_0/
│   └── openssl-3.3.2/
│
├── docs/
│   ├── ARCHITECTURE.md           ✅ This document
│   └── ARCHITECTURE_DIAGRAM.md   ✅ Visual reference
│
└── CMakeLists.txt       ✅ Clean PUBLIC/PRIVATE scoping
```

---

## Build Isolation Verification

### Step 1: Check Wallet Compilation

```bash
# Get wallet compile command
cmake --build build --target dinero_wallet --verbose 2>&1 | \
  grep "hd_wallet.cpp" | head -1

# Should NOT contain:
#   -I.../rocksdb/include  ❌
#
# Should only contain:
#   -I.../DineroCoin/include  ✅
```

### Step 2: Verify RocksDB Linkage

```bash
# Check consensus library
nm -g build/libdinero_consensus.a | grep rocksdb

# Expected: RocksDB symbols present (library uses it internally)
```

### Step 3: Verify Wallet Independence

```bash
# Check wallet library
nm -g build/libdinero_wallet.a | grep rocksdb

# Expected: No output (wallet doesn't link RocksDB) ✅
```

---

## Performance Characteristics

### Wallet Operations

| Operation | Time Complexity | Notes |
|-----------|-----------------|-------|
| DeriveNextAddress | O(1) | BIP32 key derivation |
| GetBalance | O(n) UTXOs | Filtered by maturity |
| CreateTransaction | O(n²) | Coin selection greedy |
| SignPSBT | O(m) inputs | ECDSA per input |

### Storage Operations

| Operation | Time Complexity | Notes |
|-----------|-----------------|-------|
| GetBlock | O(log n) | RocksDB B-tree lookup |
| GetUTXO | O(log n) | Indexed by outpoint |
| GetBestHeight | O(1) | Cached in memory |
| Reorg | O(k) blocks | k = depth of reorg |

### Network Operations

| Operation | Latency | Notes |
|-----------|---------|-------|
| Block relay | < 1s | Inv → getdata → block |
| Tx relay | < 100ms | Mempool propagation |
| Peer discovery | 5-30s | DNS seeds → handshake |

---

## Security Considerations

### Private Key Management

- **Storage**: Encrypted with AES-256-GCM (Argon2id-derived key)
- **Memory**: `OPENSSL_cleanse()` on sensitive data
- **Auto-lock**: Configurable timeout (default: 15 min)

### RPC Authentication

- **Cookie-based**: Generated on startup, stored in `.cookie` file
- **User/pass**: Optional override via config
- **WebSocket**: Separate auth + rate limiting (P1 TODO)

### P2P Security

- **Message validation**: All incoming data validated
- **DoS protection**: Rate limiting, ban scores
- **Eclipse attack mitigation**: Multiple DNS seeds, anchor connections

---

## Testing Strategy

### Unit Tests

```cpp
// tests/wallet/test_coinbase_maturity.cpp
TEST(CoinbaseMaturity, ImmatureAtHeight99) {
    EXPECT_FALSE(CoinbaseMaturity::isCoinbaseMature(0, 99));
}

TEST(CoinbaseMaturity, MatureAtHeight100) {
    EXPECT_TRUE(CoinbaseMaturity::isCoinbaseMature(0, 100));
}
```

### Integration Tests

```bash
# tests/integration/test_mining_maturity.sh
dinerod -regtest &
dinero-cli generate 1      # Mine coinbase
dinero-cli getbalance      # → immature: 100, confirmed: 0
dinero-cli generate 100    # Mature the coinbase
dinero-cli getbalance      # → immature: 0, confirmed: 100 ✅
```

### Stress Tests

```bash
# tests/stress/test_reorg.sh
# Mine 200 blocks, reorg 50, verify maturity recalculation
```

---

## Migration Guide (Old → New Architecture)

### Before (Legacy)

```cpp
// ❌ Wallet directly accessed ChainDB
uint32_t height = g_chain_db_direct->getTip().value().height;

// Problem: Wallet sees RocksDB headers
#include "storage/chain_db.h"  // Brings in rocksdb/db.h
```

### After (Clean DI)

```cpp
// ✅ Wallet uses abstraction
uint32_t height = chain_height_provider_->GetBestHeight();

// Clean: Only sees interface
#include "storage/chain_height_provider.h"  // Pure virtual, no RocksDB
```

---

## Glossary

- **BIP**: Bitcoin Improvement Proposal (standards)
- **ECDSA**: Elliptic Curve Digital Signature Algorithm
- **HD Wallet**: Hierarchical Deterministic wallet (BIP32)
- **PSBT**: Partially Signed Bitcoin Transaction (BIP174)
- **UTXO**: Unspent Transaction Output
- **DI**: Dependency Injection (design pattern)
- **RPC**: Remote Procedure Call (JSON-RPC 2.0)
- **P2P**: Peer-to-Peer (network protocol)
- **PoW**: Proof of Work (consensus algorithm)

---

*For implementation details, see `ARCHITECTURE.md`.*
*For contributing, see `CONTRIBUTING.md`.*
