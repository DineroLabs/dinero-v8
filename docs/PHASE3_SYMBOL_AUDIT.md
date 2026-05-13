# Phase 3: Symbol Migration Audit

**Status:** Pre-Flight Preparation
**Date:** 2026-01-07
**Purpose:** Identify all symbols that need migration from `dinero_wallet` to enable `lightningd` build independence

---

## Executive Summary

To remove `dinero_wallet` linkage from `lightningd`, we must migrate symbols that Lightning needs but are currently in wallet-only libraries.

**Total Symbols to Migrate:** ~15-20
**Destination Libraries:**
- `dinero_tx_primitives` - Transaction serialization, primitives
- `dinero_crypto` - Crypto utilities (SHA256, RIPEMD160, hex encoding)
- `dinero_lightning_stubs` - Minimal wallet interface stubs

---

## Category 1: Transaction Primitives

### Transaction::Serialize() ⚡ HIGH PRIORITY

**Current Location:** `src/wallet/transaction.cpp`
**Used By:**
- `src/lightning/channel_manager.cpp` - Funding transaction construction
- `src/lightning/commitment_builder.cpp` - Commitment transaction serialization
- `src/lightning/htlc_manager.cpp` - HTLC transaction encoding

**Destination:** `src/primitives/transaction_serializer.cpp`
**Reason:** Transaction serialization is a primitive operation, not wallet-specific

**Migration Difficulty:** LOW
**Estimated LOC:** ~200 lines (serialization logic only)

**Dependencies:**
- Depends on: `Transaction` struct (already in `include/primitives/transaction.h`)
- Used by: All transaction-handling code

**Migration Strategy:**
```cpp
// Before (wallet/transaction.cpp)
std::vector<uint8_t> Transaction::Serialize(bool include_witness) const {
    // ... implementation ...
}

// After (primitives/transaction_serializer.cpp)
namespace dinero {
std::vector<uint8_t> SerializeTransaction(const Transaction& tx, bool include_witness) {
    // ... same implementation ...
}

// Compatibility wrapper in wallet/transaction.cpp
std::vector<uint8_t> Transaction::Serialize(bool include_witness) const {
    return dinero::SerializeTransaction(*this, include_witness);
}
}
```

**Rollback Point:** Keep old method as wrapper for 1 release cycle

---

## Category 2: Crypto Utilities

### DoubleSHA256() / DoubleSHA256Bytes()

**Current Location:** `src/wallet/transaction.cpp` (inline helper)
**Used By:**
- Lightning transaction hashing
- HTLC preimage verification
- Commitment transaction IDs

**Destination:** `src/crypto/hash_utils.cpp`
**Estimated LOC:** ~30 lines

**Implementation:**
```cpp
// hash_utils.h
namespace dinero {
namespace crypto {
    std::vector<uint8_t> DoubleSHA256(const std::vector<uint8_t>& data);
    uint256 DoubleSHA256Hash(const std::vector<uint8_t>& data);
}
}
```

---

### ToHex() / FromHex()

**Current Location:** Various locations (`src/wallet/transaction.cpp`, others)
**Used By:**
- Transaction ID encoding
- Debug logging in Lightning
- RPC serialization

**Destination:** `src/crypto/hex_encoding.cpp`
**Estimated LOC:** ~50 lines

**Implementation:**
```cpp
// hex_encoding.h
namespace dinero {
namespace crypto {
    std::string ToHex(const std::vector<uint8_t>& data);
    std::vector<uint8_t> FromHex(const std::string& hex);
}
}
```

---

### RIPEMD160()

**Current Location:** `src/lightning/crypto_utils.cpp` (already extracted!)
**Status:** ✅ Already in Lightning library
**Action:** Move to `dinero_crypto` for shared use

**Destination:** `src/crypto/hash_utils.cpp`

---

## Category 3: Wallet Interface Symbols

### WalletManager::listUnspentUTXOs()

**Current Usage Count:** 4 call sites in Lightning code
**Call Sites:**
- `src/lightning/channel_manager.cpp:153` - Funding UTXO selection
- `src/lightning/channel_manager.cpp:999` - Force-close UTXO lookup
- `src/lightning/lightning_wallet.cpp:156` - Available UTXO query
- `src/lightning/lightning_wallet.cpp:459` - Min-confirmations UTXO query

**Phase 2 Status:** ✅ Already wrapped in `WalletClient::listUnspentUTXOs()` via gRPC
**Phase 3 Action:** Keep gRPC client, remove direct wallet linkage

**Stub Implementation:**
```cpp
// lightning/wallet_manager_stub.cpp
namespace dinero {
std::vector<WalletManager::UTXO> WalletManager::listUnspentUTXOs(int min, int max) const {
    throw std::runtime_error("WalletManager stub called - use WalletClient in lightningd mode");
}
}
```

---

### HDWallet::GetLightning*KeyAt() - 5 Methods

**Methods to Stub:**
1. `GetLightningFundingKeyAt(uint32_t index)`
2. `GetLightningRevocationBaseKeyAt(uint32_t index)`
3. `GetLightningPaymentBaseKeyAt(uint32_t index)`
4. `GetLightningDelayedPaymentBaseKeyAt(uint32_t index)`
5. `GetLightningHTLCBaseKeyAt(uint32_t index)`

**Current Usage:** All wrapped in Phase 2 via `WalletClient`
**Call Sites:** `src/lightning/lightning_wallet.cpp:357-377`

**Phase 3 Action:** Create minimal stub that throws at runtime

**Stub Implementation:**
```cpp
// lightning/hd_wallet_stub.cpp
std::vector<uint8_t> HDWallet::GetLightningFundingKeyAt(uint32_t index) const {
    throw std::runtime_error("HDWallet stub called - use WalletClient");
}
// ... repeat for 4 other methods
```

---

## Category 4: Type Definitions

### UTXO Type Mismatch ⚠️ CRITICAL BUG

**Problem:** Lightning uses `WalletManager::UTXO`, but this type doesn't exist
**Current Error:**
```
error: no type named 'UTXO' in 'dinero::WalletManager'
```

**Root Cause:** Type confusion between:
- `dinero::SigningUTXO` (primitives/transaction.h) - cryptographic primitive
- `UTXOIndex::Entry` (wallet/utxo_index.h) - wallet database entry
- Lightning expects a third type with fields: `txid`, `vout`, `amount`, `scriptPubKey`, `confirmations`

**Solution Options:**

**Option A: Define Lightning-specific UTXO type**
```cpp
// include/lightning/lightning_types.h
namespace dinero {
namespace lightning {
struct UTXO {
    uint256 txid;
    uint32_t vout;
    uint64_t amount;
    std::vector<uint8_t> scriptPubKey;
    int confirmations;
};
}
}
```

**Option B: Use gRPC proto UTXO type** (RECOMMENDED)
```protobuf
// proto/dinerod.proto (already exists)
message UTXO {
    string txid = 1;
    uint32 vout = 2;
    uint64 amount = 3;
    bytes scriptPubKey = 4;
    int32 confirmations = 5;
}
```

**Recommended:** Option B - reuse proto types, enforce gRPC boundary

---

## Migration Checklist by Symbol

### Week 1: Symbol Inventory & Interface Design

- [ ] **Transaction::Serialize()**
  - [ ] Create `src/primitives/transaction_serializer.cpp`
  - [ ] Add to `dinero_tx_primitives` CMake target
  - [ ] Create compatibility wrapper in `wallet/transaction.cpp`
  - [ ] Update all call sites to use new location
  - [ ] Test: All transaction tests pass

- [ ] **DoubleSHA256() / ToHex() / FromHex()**
  - [ ] Create `src/crypto/hash_utils.cpp`
  - [ ] Create `src/crypto/hex_encoding.cpp`
  - [ ] Add to `dinero_crypto` CMake target
  - [ ] Update all call sites
  - [ ] Test: Crypto tests pass

- [ ] **UTXO Type Definition**
  - [ ] Fix Lightning to use proto UTXO type from gRPC
  - [ ] Remove all `WalletManager::UTXO` references
  - [ ] Update `lightning_wallet.cpp` to use `dinerod::UTXO` (proto type)
  - [ ] Test: Lightning compiles

### Week 2: Stub Implementation

- [ ] **WalletManager Stubs**
  - [ ] Create `src/lightningd/wallet_manager_stub.cpp`
  - [ ] Implement `listUnspentUTXOs()` stub (throws exception)
  - [ ] Add to `lightningd` CMake target (not `dinero_core`)
  - [ ] Test: Stub compiles, throws at runtime

- [ ] **HDWallet Stubs**
  - [ ] Create `src/lightningd/hd_wallet_stub.cpp`
  - [ ] Implement 5 `GetLightning*KeyAt()` stubs
  - [ ] Add to `lightningd` CMake target
  - [ ] Test: Stub compiles, throws at runtime

### Week 3: Build System Changes

- [ ] **Remove dinero_wallet from lightningd**
  - [ ] Update `CMakeLists.txt` to remove `dinero_wallet` from `target_link_libraries(lightningd)`
  - [ ] Verify `lightningd` links only: `dinerod_proto`, `dinero_tx_primitives`, `lightning_core_static`
  - [ ] Test: `lightningd` builds successfully
  - [ ] Test: Binary size reduced by ~5-10 MB

---

## Symbol Count Summary

| Category | Symbol Count | Destination Library | Migration Week |
|----------|--------------|---------------------|----------------|
| Transaction Primitives | 1 | `dinero_tx_primitives` | Week 1 |
| Crypto Utilities | 4 | `dinero_crypto` | Week 1 |
| Wallet Interface (stubs) | 6 | `lightningd` stubs | Week 2 |
| Type Definitions | 1 | Use proto types | Week 1 |
| **TOTAL** | **12** | Multiple | 3 weeks |

---

## Risk Assessment

| Symbol | Risk Level | Reason | Mitigation |
|--------|-----------|--------|------------|
| Transaction::Serialize() | LOW | Well-defined, stateless | Keep wrapper for backward compat |
| DoubleSHA256() | LOW | Pure function | Direct move |
| ToHex() / FromHex() | LOW | Pure function | Direct move |
| UTXO Type | **HIGH** | Type confusion, compilation blocker | Fix with proto types first |
| WalletManager stubs | MEDIUM | Runtime stubs never called in lightningd | Add tests to verify |
| HDWallet stubs | MEDIUM | Runtime stubs never called in lightningd | Add tests to verify |

---

## Next Steps

1. **Fix UTXO Type Mismatch FIRST** (blocks compilation)
   - Update Lightning to use `dinerod::UTXO` proto type
   - Remove all `WalletManager::UTXO` references
   - Verify Lightning compiles with `ENABLE_LIGHTNING=ON`

2. **Create Symbol Migration PRs** (one per week)
   - Week 1: Transaction primitives + crypto
   - Week 2: Stubs
   - Week 3: Build system cleanup

3. **Verification**
   - Run full test suite after each migration
   - Verify binary size reduction
   - Ensure runtime behavior unchanged

---

**Document Status:** Pre-Flight Audit
**Owner:** DineroCoin Core Team
**Review Cycle:** Before Phase 3 starts
