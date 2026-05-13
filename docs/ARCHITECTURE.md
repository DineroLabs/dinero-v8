# DineroCoin Architecture

**Post-Refactor Clean Architecture (v0.6.1)**
*Last Updated: 2025-02-02*

---

## Process Separation (Non-Negotiable)

**Dinero enforces strict process separation.** Consensus code (`dinerod`) must not embed or link against GUI, mining pools, or auxiliary services. All non-consensus components interact exclusively via RPC.

```
┌─────────────────┐         RPC          ┌──────────────────┐
│    dinerod      │◄────────────────────►│   dinero-qt      │
│   (consensus)   │                      │   (GUI wallet)   │
│                 │         RPC          ├──────────────────┤
│  - Validation   │◄────────────────────►│  dinero-stratum  │
│  - P2P network  │                      │  (mining server) │
│  - Mempool      │                      └──────────────────┘
│  - Wallet RPC   │
└─────────────────┘
```

### Repository Structure

```
~/src/
├── dinero/          # Consensus daemon (C++) - THIS REPO
│   └── dinerod      # MUST NOT contain Qt, Stratum, or GUI code
│
├── dinero-qt/       # Desktop wallet (Qt6) - SEPARATE REPO
│   └── Connects via RPC only
│
├── stratum/         # Mining server - SEPARATE REPO
│   └── Connects via RPC only
```

### Forbidden Dependencies in dinerod

- **No Qt** - GUI belongs in `dinero-qt/`
- **No Stratum** - Mining server belongs in `stratum/`
- **No embedded services** - All auxiliary features use RPC

### CI Enforcement

These invariants are enforced by CI and must never be disabled:

```bash
# Fails build if GUI code found in consensus
grep -rE "(QApplication|QWidget|QMainWindow)" src/ && exit 1

# Fails build if Stratum embedded in consensus
grep -rE "class StratumServer" src/ && exit 1
```

---

## Overview

DineroCoin follows a **layered architecture** pattern inspired by Bitcoin Core but modernized with dependency injection and strict module isolation. This document describes the post-refactor architecture where RocksDB is cleanly isolated and wallet logic operates independently of storage implementation details.

## Core Principles

1. **Dependency Inversion**: High-level modules (wallet) don't depend on low-level modules (storage)
2. **Interface Segregation**: Clean abstractions separate concerns (e.g., `ChainHeightProvider`)
3. **Single Responsibility**: Each module has one well-defined purpose
4. **Build Hygiene**: CMake PUBLIC/PRIVATE scoping prevents header pollution

---

## Layer Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   GUI (dinero-qt)                       │
│              Qt6 Widgets | WebSocket UI                 │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────┴────────────────────────────────────┐
│                  Daemon (dinerod)                       │
│        RPC Server | P2P Manager | Mining Coordinator    │
└────┬─────────────┬──────────────┬──────────────┬────────┘
     │             │              │              │
┌────┴─────┐ ┌────┴──────┐ ┌─────┴──────┐ ┌────┴─────────┐
│  Wallet  │ │ Consensus │ │  Storage   │ │  Networking  │
│  Layer   │ │   Layer   │ │   Layer    │ │    Layer     │
└──────────┘ └───────────┘ └────────────┘ └──────────────┘
```

### Layer Dependencies (Bottom-Up)

```
Storage (RocksDB Backend)
   ↑
Consensus (Block Validation, Difficulty, Coinbase Rules)
   ↑
Wallet (HD Keys, PSBT, Balance)  ←  ChainHeightProvider (DI)
   ↑
RPC/Daemon (User Interface)
   ↑
GUI (Qt6 Frontend)
```

---

## Module Descriptions

### 1. Storage Layer (`dinero_consensus` + RocksDB)

**Purpose**: Persistent blockchain state management

**Key Components**:
- `ChainDB` - RocksDB wrapper for blocks, UTXOs, and chain state
- `ChainHeightProvider` - Clean interface abstracting chain tip access
- `UTXOIndex` - Tracks unspent transaction outputs

**Dependencies**:
- RocksDB (PRIVATE linkage - headers isolated to .cpp files only)
- SQLite3 (for wallet transaction history)

**CMake Target**: `dinero_consensus`

**Critical Design Decision**: RocksDB headers are **NEVER** exposed publicly. Only implementation (.cpp) files see RocksDB types. This prevents namespace pollution and allows wallet/RPC layers to build independently.

---

### 2. Consensus Layer

**Purpose**: Blockchain validation and consensus rules

**Key Components**:
- `CoinbaseMaturity` - 100-block maturity enforcement
- `ASERT` - Difficulty adjustment algorithm
- `BlockAcceptor` - Block validation pipeline
- `TransactionValidator` - Transaction verification

**Dependencies**:
- Crypto primitives (secp256k1, SHA256, RIPEMD160)
- JsonCpp (for parameter serialization)

**CMake Target**: `dinero_consensus`

**No RocksDB Dependency**: Consensus logic operates on in-memory block structures, not database specifics.

---

### 3. Wallet Layer (`dinero_wallet`)

**Purpose**: Key management, transaction creation, balance tracking

**Key Components**:
- `HDWallet` - BIP32/BIP39/BIP84 hierarchical deterministic wallet
- `PSBT` - Partially Signed Bitcoin Transactions (BIP174)
- `WalletManager` - Multi-wallet coordination
- `Address` - Bech32 address encoding/decoding

**Chain Height Access**: Via `ChainHeightProvider` interface (dependency injection)

**Balance Segregation**:
```cpp
struct WalletBalance {
    uint64_t confirmed;   // Spendable mature coins
    uint64_t immature;    // Coinbase < 100 confirmations
    uint64_t total;       // confirmed + immature
};
```

**CMake Target**: `dinero_wallet`

**Critical Achievement**: Wallet **NEVER** includes RocksDB headers. It accesses chain state through clean interfaces only.

---

### 4. RPC/Daemon Layer

**Purpose**: User-facing API and network coordination

**Key Components**:
- `HttpRpcServer` - JSON-RPC 2.0 server
- `WsServer` - WebSocket notification system
- `P2PManager` - Peer-to-peer networking
- `TransactionPool` - Mempool management

**RPC Registry**: Modern modular design replacing legacy RPC handlers

**Dependencies**:
- Boost.Asio (for WebSockets)
- All lower layers (wallet, consensus, storage)

**CMake Target**: `dinerod`

---

### 5. GUI Layer (Optional)

**Purpose**: Desktop wallet application

**Key Components**:
- `MainWindow` - Qt6 Widgets-based UI
- `TransactionView` - Transaction history display
- `SendCoinsDialog` - Payment interface

**Dependencies**:
- Qt6 Widgets
- Daemon RPC client

**CMake Target**: `dinero-qt`

---

## Dependency Injection Pattern

### Problem Solved

The wallet needs to know the current blockchain height to enforce coinbase maturity rules, but it should NOT depend on RocksDB directly. This creates a circular dependency:

```
❌ BAD (Circular):
Wallet → ChainDB → RocksDB
  ↑         ↓
  └─────────┘
```

### Solution: ChainHeightProvider Interface

```cpp
// include/storage/chain_height_provider.h
namespace dinero {

class ChainHeightProvider {
public:
    virtual ~ChainHeightProvider() = default;
    virtual uint32_t GetBestHeight() const = 0;
    virtual bool IsAvailable() const = 0;
};

// Global singleton (set once at daemon startup)
ChainHeightProvider* GetGlobalChainHeightProvider();
void SetGlobalChainHeightProvider(ChainHeightProvider* provider);

} // namespace dinero
```

### Implementation (Isolated in storage layer)

```cpp
// src/storage/chain_height_provider.cpp
#include "storage/chain_db.h"  // RocksDB headers HERE ONLY

class ChainDBHeightProvider : public ChainHeightProvider {
    ChainDB* chain_db_;
public:
    uint32_t GetBestHeight() const override {
        return chain_db_->getTip().value().height;
    }
};
```

### Wallet Usage (Clean)

```cpp
// src/wallet/hd_wallet.cpp - NO RocksDB headers!
#include "storage/chain_height_provider.h"  // Clean interface only

WalletBalance HDWallet::GetBalance() const {
    uint32_t current_height = 0;
    if (chain_height_provider_ && chain_height_provider_->IsAvailable()) {
        current_height = chain_height_provider_->GetBestHeight();
    }

    for (const auto& utxo : utxos) {
        if (utxo.is_coinbase) {
            if (CoinbaseMaturity::isCoinbaseMature(utxo.height, current_height)) {
                balance.confirmed += utxo.value;  // Mature
            } else {
                balance.immature += utxo.value;   // Not yet mature
            }
        }
    }
}
```

**Result**: Clean layering with no circular dependencies ✅

---

## CMake Build Isolation

### Target Dependency Graph

```
dinerod (executable)
  ├─ dinero_wallet (PRIVATE)
  ├─ dinero_consensus (PRIVATE)
  ├─ dinero_rpc_handlers (PRIVATE)
  └─ boost_system_vendored (PRIVATE)

dinero_wallet (library)
  ├─ dinero_crypto (PRIVATE)
  └─ [NO RocksDB]

dinero_consensus (library)
  ├─ dinero_crypto (PUBLIC)
  ├─ jsoncpp_static (PUBLIC)
  ├─ secp256k1 (PUBLIC)
  └─ rocksdb (PRIVATE)  ← Key isolation!

dinero_rpc_handlers (library)
  ├─ dinero_wallet (PUBLIC)
  ├─ dinero_consensus (PUBLIC)
  └─ rocksdb includes (PRIVATE) ← Only for chain_db.h usage
```

### Critical CMake Configuration

```cmake
# dinero_consensus: RocksDB is PRIVATE (headers not propagated)
target_link_libraries(dinero_consensus
  PUBLIC
    dinero_crypto
    jsoncpp_static
    secp256k1
    sqlite3
  PRIVATE
    ${ROCKSDB_TARGET}  # ← Prevents header pollution
)

# dinero_wallet: No RocksDB at all
target_link_libraries(dinero_wallet
  PRIVATE
    dinero_consensus
    dinero_crypto
)

# dinero_rpc_handlers: RocksDB includes (not linkage) for chain_db.h
target_include_directories(dinero_rpc_handlers
  PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb-9.1.1/include
)
```

---

## Coinbase Maturity Implementation

### Consensus Rule

Coinbase outputs (mining rewards) cannot be spent until they have **100 confirmations**. This prevents issues during blockchain reorganizations where coinbase transactions might become invalid.

### Constant Definition

```cpp
// include/consensus/coinbase_maturity.h
namespace dinero {

class CoinbaseMaturity {
public:
    static constexpr uint32_t COINBASE_MATURITY = 100;

    static bool isCoinbaseMature(uint32_t coinbase_height,
                                 uint32_t current_height) {
        if (current_height < coinbase_height) return false;
        return (current_height - coinbase_height) >= COINBASE_MATURITY;
    }
};

} // namespace dinero
```

### Integration Points

1. **Wallet Balance** (`hd_wallet.cpp:682-720`)
   - Segregates mature vs immature coinbase rewards
   - Uses `ChainHeightProvider` for current height

2. **Transaction Validation** (`transaction_validator.cpp`)
   - Rejects transactions spending immature coinbase
   - Checks all inputs for maturity

3. **RPC Responses** (`getbalance`, `listunspent`)
   - Shows separate `mature` and `immature` balances
   - Flags individual UTXOs as mature/immature

---

## Critical Bug Fixes (Post-Refactor)

### Bug #1: Missing Namespace Closure

**File**: `include/consensus/coinbase_maturity.h`

**Problem**: Header opened `namespace dinero {` but never closed it, causing all subsequent includes (like `<filesystem>`) to be pulled into the `dinero` namespace.

**Symptom**:
```
error: no template named 'time_point' in namespace 'dinero::std::chrono'
```

**Fix**: Added missing `} // namespace dinero` at end of file

---

### Bug #2: RocksDB PUBLIC Linkage

**Problem**: RocksDB was linked PUBLIC in `dinero_consensus`, propagating its include paths to all consumers (wallet, RPC, GUI).

**Symptom**: Wallet compilation saw RocksDB headers, causing namespace `std` pollution from RocksDB's macro-heavy headers.

**Fix**: Changed all three platform-specific linkages (Apple, Windows, Linux) from PUBLIC to PRIVATE:

```cmake
# Before (BAD):
target_link_libraries(dinero_consensus PUBLIC ${ROCKSDB_TARGET})

# After (GOOD):
target_link_libraries(dinero_consensus
  PUBLIC secp256k1 jsoncpp_static
  PRIVATE ${ROCKSDB_TARGET}  # ← Isolated!
)
```

---

### Bug #3: Legacy RPC Handler

**File**: `src/core/rpc/validation_rpc_handlers.cpp`

**Problem**: File used old `HttpRpcServer` API which is disabled (`DIN_ENABLE_LEGACY_RPC=OFF`).

**Fix**: Removed from build in `CMakeLists.txt` and commented out registration in `main.cpp`. Modern RPC registry handles validation commands.

---

## Build Verification

### Full Build Success

```bash
$ cmake --build build --target dinerod -j8
[100%] Built target dinero_wallet
[100%] Built target dinero_consensus
[100%] Built target dinero_rpc_handlers
[100%] Built target dinerod
```

### Dependency Hygiene Audit

To verify RocksDB isolation:

```bash
# Wallet should NOT have rocksdb in compile commands
cmake --build build --target dinero_wallet --verbose 2>&1 | grep rocksdb
# → Expected: No output ✅

# Consensus should have rocksdb (PRIVATE)
cmake --build build --target dinero_consensus --verbose 2>&1 | grep rocksdb
# → Expected: rocksdb linked, but includes not exported ✅
```

---

## Future Improvements

### Short-Term (Next Milestone)

1. **RPC Maturity Display**
   - Add `"mature": true/false` field to `listunspent` output
   - Show `immature` balance in `getbalance` response

2. **Integration Tests**
   - Test suite for coinbase maturity edge cases
   - Regression tests for balance segregation

3. **WebSocket Enhancements** (P1 Roadmap)
   - Authentication with cookie validation
   - Rate limiting
   - Backpressure handling

### Long-Term

1. **Full Modular CMake**
   - Split monolithic `CMakeLists.txt` into subdirectory structure:
     ```
     src/wallet/CMakeLists.txt
     src/consensus/CMakeLists.txt
     src/storage/CMakeLists.txt
     ```

2. **Abstract Storage Layer**
   - Support multiple backends (RocksDB, LevelDB, in-memory)
   - Plugin architecture for storage engines

3. **Hardware Wallet Support**
   - PSBT-based signing workflow complete
   - Need USB HID communication layer

---

## References

### Standards Implemented

- **BIP32**: Hierarchical Deterministic Wallets
- **BIP39**: Mnemonic Code for Generating Deterministic Keys
- **BIP84**: Derivation scheme for P2WPKH based accounts
- **BIP141**: Segregated Witness (for future upgrade)
- **BIP174**: Partially Signed Bitcoin Transaction Format

### Similar Projects

- **Bitcoin Core**: Reference implementation, but lacks clean DI patterns
- **Elements Project**: Sidechain implementation with modular architecture
- **btcd**: Go implementation with excellent layering

---

## Changelog

### v0.6.1 (2025-11-02) - Architecture Stabilization

**Added**:
- ✅ DNS seed resolution with IPv4/IPv6 support
- ✅ `ChainHeightProvider` dependency injection pattern
- ✅ Coinbase maturity balance segregation
- ✅ Clean CMake PUBLIC/PRIVATE scoping

**Fixed**:
- ✅ Missing namespace closure in `coinbase_maturity.h`
- ✅ RocksDB header pollution via PUBLIC linkage
- ✅ Legacy RPC handler build errors

**Removed**:
- ❌ `validation_rpc_handlers.cpp` (legacy RPC system)
- ❌ Direct RocksDB access from wallet layer

---

## Contributors

Architecture designed and implemented following Bitcoin Core patterns with modern C++17 improvements and dependency injection principles.

For questions or improvements, see `CONTRIBUTING.md`.
