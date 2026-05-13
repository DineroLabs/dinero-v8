# Phase 3: Lightning Build-Time Decoupling Plan

**Status:** ✅ PREPARATION COMPLETE (Ready for Execution)
**Prerequisites:** Phase 2 testing complete, mainnet stable, Utreexo audited
**Duration Estimate:** 3-4 weeks
**Risk Level:** MEDIUM (symbol moves, ABI changes)
**Last Updated:** 2026-01-07

---

## 🎯 Preparation Status

**Phase 3 Planning: COMPLETE** ✅

All preparation documents ready:
- ✅ `PHASE3_SYMBOL_AUDIT.md` - 12 symbols identified, migration plan ready
- ✅ `PHASE3_COMMIT1_CHECKLIST.md` - First commit (UTXO fix) ready to execute
- ✅ `PHASE3_LIGHTNINGD_BINARY_LAYOUT.md` - Final binary architecture defined
- ✅ `PHASE3_PREFLIGHT_CHECKLIST.md` - 50+ gate-checks before starting
- ✅ `PHASE3_PREPARATION_SUMMARY.md` - Executive summary

**See:** `docs/PHASE3_PREPARATION_SUMMARY.md` for quick start guide

---

## Goal

Remove `dinero_wallet` library dependency from `lightningd` build target, achieving complete build-time separation while maintaining runtime correctness.

**Success Criteria:**
```cmake
# Before (current)
target_link_libraries(lightningd PRIVATE dinero_wallet ...)

# After (Phase 3)
target_link_libraries(lightningd PRIVATE
    dinerod_proto
    dinero_tx_primitives
    lightning_core_static
    # NO dinero_wallet
)
```

---

## Pre-Flight Checklist

Before starting Phase 3, ensure:

- [ ] Phase 2 testing complete (all L1/L2 integration tests pass)
- [ ] Mainnet running stable for 2+ weeks
- [ ] Utreexo audit findings addressed
- [ ] No critical bugs in Lightning implementation
- [ ] Regression test suite established (baseline)
- [ ] Branch created: `phase3/lightning-build-decoupling`

---

## Commit-Sized Steps (Atomic, Testable)

### Week 1: Symbol Inventory & Extraction Plan

#### Commit 1: Audit dinero_wallet Dependencies
**File:** `docs/PHASE3_SYMBOL_AUDIT.md`
**Action:** Generate complete list of symbols `lightningd` requires from `dinero_wallet`
**Commands:**
```bash
# Extract undefined symbols
nm -u build/bin/lightningd | grep dinero_wallet | sort > symbols_needed.txt

# Cross-reference with wallet library
nm -g build/lib/libdinero_wallet.a | grep " T " > wallet_exports.txt

# Generate migration list
comm -12 symbols_needed.txt wallet_exports.txt > migration_list.txt
```
**Expected Symbols:**
- `Transaction::Serialize()`
- `WalletManager::listUnspentUTXOs()`
- `HDWallet::GetLightning*KeyAt()` (5 methods)
- Common crypto utilities
- Database helpers

**Deliverable:** Migration list with destination library for each symbol
**Test:** None (documentation only)

---

#### Commit 2: Create dinero_wallet_api Header-Only Interface
**File:** `include/wallet/wallet_api.h`
**Action:** Define pure virtual interface for wallet operations
**Code:**
```cpp
namespace dinero {

// Pure interface - no implementation dependencies
class IWalletOperations {
public:
    virtual ~IWalletOperations() = default;

    virtual std::vector<UTXO> listUnspentUTXOs(int minConf, int maxConf = 9999999) const = 0;
    virtual std::vector<uint8_t> deriveLightningKey(KeyType type, uint32_t index) const = 0;
    virtual bool signTransaction(Transaction& tx, const std::vector<UTXO>& utxos) = 0;
};

} // namespace dinero
```
**Build Change:** Add to `dinero_tx_primitives` (header-only)
**Test:** Verify `lightningd` compiles with new header included
**Rollback:** Remove header, revert include

---

### Week 2: Transaction Primitives Extraction

#### Commit 3: Move Transaction::Serialize() to Primitives
**Files:**
- `src/primitives/transaction_serializer.cpp` (NEW)
- `src/wallet/transaction.cpp` (REMOVE Serialize())

**Action:**
1. Copy `Transaction::Serialize()` implementation to new file
2. Add to `dinero_tx_primitives` CMake target
3. Keep original as `deprecated` wrapper (calls new version)
4. Update all callers to use new location

**Build Changes:**
```cmake
# CMakeLists.txt
add_library(dinero_tx_primitives STATIC
  src/primitives/transaction.cpp
  src/primitives/transaction_serializer.cpp  # NEW
  src/primitives/schnorr_signer.cpp
  src/primitives/taproot_tx_signer.cpp
)
```

**Test:**
```bash
# Verify serialization matches old version
./tests/test_transaction_serialization
./tests/test_lightning_funding_tx
```

**Rollback:** `git revert HEAD` (old wrapper still works)

---

#### Commit 4: Extract Common Crypto Utilities
**Files:**
- `src/crypto/hash_utils.cpp` (NEW)
- `include/crypto/hash_utils.h` (NEW)

**Action:** Move shared hash functions from `src/wallet/transaction.cpp`:
- `DoubleSHA256()`
- `DoubleSHA256Bytes()`
- `ToHex()`, `FromHex()`

**Build Changes:**
```cmake
add_library(dinero_crypto STATIC
  # existing files...
  src/crypto/hash_utils.cpp  # NEW
)
```

**Test:**
```bash
./tests/test_hash_utils
./tests/test_transaction_hashing
```

**Rollback:** `git revert HEAD`

---

### Week 3: Lightning Wallet Stubs

#### Commit 5: Create Minimal WalletManager Stub
**File:** `src/lightningd/wallet_manager_stub.cpp`
**Action:** Provide stub implementations for symbols Lightning binary needs
```cpp
namespace dinero {

// Stub - should never be called in lightningd mode
std::vector<WalletManager::UTXO> WalletManager::listUnspentUTXOs(int, int) const {
    throw std::runtime_error("WalletManager stub called - use WalletClient");
}

// ... other stubs
}
```

**Build Changes:**
```cmake
add_executable(lightningd
  # existing sources...
  src/lightningd/wallet_manager_stub.cpp  # NEW
)
```

**Test:**
```bash
# Verify stub is never called at runtime
./tests/test_lightningd_wallet_isolation
```

**Rollback:** Remove stub file

---

#### Commit 6: Create Minimal HDWallet Stub
**File:** `src/lightningd/hd_wallet_stub.cpp`
**Action:** Stub out HDWallet methods Lightning needs
```cpp
// Global scope (HDWallet is not in dinero namespace)

std::vector<uint8_t> HDWallet::GetLightningFundingKeyAt(uint32_t) const {
    throw std::runtime_error("HDWallet stub called - use WalletClient");
}

// ... 4 more key derivation stubs
```

**Test:**
```bash
./tests/test_lightningd_key_derivation_isolation
```

**Rollback:** Remove stub file

---

#### Commit 7: Remove dinero_wallet from lightningd Link
**File:** `CMakeLists.txt`
**Action:** Remove `dinero_wallet` from `target_link_libraries(lightningd ...)`

**Build Changes:**
```cmake
target_link_libraries(lightningd PRIVATE
    dinerod_proto
    dinero_tx_primitives
    lightning_core_static
    # dinero_wallet REMOVED
    rocksdb
    sqlite3
    jsoncpp_static
    OpenSSL::Crypto
    pthread
    ${CMAKE_DL_LIBS}
)
```

**Test:**
```bash
# Must pass - critical checkpoint
cmake --build build --target lightningd -j8
./tests/test_lightningd_startup
./tests/test_lightning_channel_lifecycle
```

**Rollback:** Re-add `dinero_wallet` to link line

**This is the pivotal commit - if this passes, build-time decoupling is achieved.**

---

### Week 4: Validation & Cleanup

#### Commit 8: Verify No Wallet Symbols in lightningd Binary
**Script:** `scripts/verify_lightningd_independence.sh`
**Action:**
```bash
#!/bin/bash
# Fail if lightningd binary contains wallet symbols it shouldn't

FORBIDDEN_SYMBOLS=(
    "WalletManager::createTransaction"
    "HDWallet::Unlock"
    "UTXOIndex::"
)

for sym in "${FORBIDDEN_SYMBOLS[@]}"; do
    if nm build/bin/lightningd | grep -q "$sym"; then
        echo "ERROR: Found forbidden symbol: $sym"
        exit 1
    fi
done

echo "✅ lightningd binary is wallet-independent"
```

**Test:** Run script in CI
**Rollback:** N/A (verification only)

---

#### Commit 9: Update Documentation
**Files:**
- `docs/ARCHITECTURE_LIGHTNING_SEPARATION.md` (update status)
- `README.md` (update build instructions)
- `docs/BUILD.md` (update dependency graph)

**Action:** Mark Phase 3 as complete, update architecture diagrams

**Test:** Documentation builds, links work

---

#### Commit 10: Add Regression Test Suite
**File:** `tests/integration/test_build_isolation.cpp`
**Action:** Automated test that verifies:
1. `lightningd` does not link `dinero_wallet`
2. Runtime wallet access goes through gRPC only
3. Stubs throw if accidentally called

**Test:**
```bash
./tests/integration/test_build_isolation
```

---

## Rollback Plan (If Phase 3 Fails)

### Emergency Rollback
```bash
git checkout main
git branch -D phase3/lightning-build-decoupling
# Revert to Phase 2 state - runtime decoupling still works
```

### Partial Rollback (Keep Completed Commits)
```bash
# Revert specific commits while keeping earlier work
git revert <commit-hash> --no-edit
```

**Safe Rollback Points:**
- After Commit 2: Interface added, no breakage
- After Commit 4: Crypto extracted, old code still works
- After Commit 6: Stubs added, wallet still linked

**Dangerous Rollback:**
- After Commit 7: Must re-add `dinero_wallet` to link line immediately

---

## Testing Strategy

### Per-Commit Tests
Every commit must pass:
```bash
# Build both daemons
cmake --build build --target dinerod -j8
cmake --build build --target lightningd -j8

# Run minimal smoke test
./tests/test_daemon_startup
./tests/test_lightning_basic
```

### Final Validation (After Commit 10)
```bash
# Full test suite
./tests/run_all.sh

# Integration tests
./tests/integration/test_lightning_mainnet_simulation
./tests/integration/test_channel_lifecycle
./tests/integration/test_force_close_recovery

# Symbol verification
./scripts/verify_lightningd_independence.sh

# Performance regression check
./tests/benchmark/lightning_benchmark.sh > phase3_results.txt
diff phase2_baseline.txt phase3_results.txt
```

**Acceptance Criteria:**
- All tests pass
- No performance degradation >5%
- Binary size reduction confirmed
- No new warnings/errors

---

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| ABI breakage | MEDIUM | HIGH | Incremental commits, keep old wrappers |
| Symbol resolution failure | LOW | HIGH | Stub library provides fallbacks |
| Runtime crash | LOW | CRITICAL | Extensive testing at each step |
| Performance regression | LOW | MEDIUM | Benchmark at each commit |
| Merge conflicts | MEDIUM | LOW | Rebase frequently, small commits |

**Overall Risk:** MEDIUM
**Recommended Schedule:** 4 weeks with daily checkpoints

---

## Success Metrics

- [ ] `lightningd` binary does not link `dinero_wallet`
- [ ] All runtime wallet access via gRPC (verified by logging)
- [ ] No new test failures vs. Phase 2 baseline
- [ ] Binary size reduction: ~5-10 MB expected
- [ ] Build time reduction: ~10-15% expected
- [ ] Symbol count reduction: ~200-300 symbols removed

---

## Post-Phase 3: External Lightning Integration

Once build-time decoupling completes, external Lightning implementations can integrate:

### Example: Custom Lightning Client
```bash
# Only needs gRPC client, no DineroCoin libraries
git clone https://github.com/dinerocoin/grpc-proto
protoc --cpp_out=. dinerod.proto
# Implement custom Lightning using WalletService RPC
```

**Benefits:**
- Alternative Lightning UX/features
- Independent security audits
- Community contributions
- Research implementations (e.g., Eltoo, PTLCs)

---

## Conclusion

Phase 3 is **refactoring, not validation** - it improves architecture without changing behavior.

**Timing is critical:** Only execute after Phase 2 stabilization confirms runtime correctness.

**Execution discipline:** Small commits, frequent tests, clear rollback points.

**End state:** `lightningd` is a pure L2 client, dinerod is a pure L1 node, boundary is gRPC.

---

**Document Status:** PLANNED - Do not execute until Phase 2 complete
**Owner:** DineroCoin Core Team
**Review Required:** Before starting Week 1
**Update Cycle:** Weekly during execution
