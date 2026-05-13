# Wallet ↔ Mempool Interface Contract

**Version**: v0.12.0
**Status**: ⚠️ **FROZEN** - Do not modify without MAJOR version bump
**Effective Date**: 2025-12-14
**Review Required**: v0.13.0 or later (if breaking changes needed)

---

## Contract Purpose

This document defines the **permanent, stable boundary** between:
- **Wallet subsystem** (key management, coin selection, user intent)
- **Mempool subsystem** (policy enforcement, relay, mining priority)

### Why This Contract Exists

1. **Prevents Abstraction Leakage**
   - Wallet must never depend on mempool internals
   - Mempool must never depend on wallet internals

2. **Enables Independent Evolution**
   - Relay protocols can change without breaking wallet
   - Fee estimator can be rewritten without touching mempool core
   - Hardware wallets can integrate cleanly

3. **Matches Bitcoin Core Layering**
   - Proven architecture with 15+ years of evolution
   - Clear responsibility boundaries

4. **Survives Network Changes**
   - P2P relay changes (compact blocks, package relay)
   - Policy updates (v3 transactions, ephemeral anchors)
   - Future soft/hard forks

---

## Contract Signatories

### Wallet Subsystem

**Responsibilities**:
- HD key management (BIP32/BIP44)
- Private key storage and encryption
- Coin selection algorithms
- Transaction construction (unsigned)
- Transaction signing
- UTXO state tracking
- User intent interpretation

**Does NOT**:
- Enforce mempool policy
- Calculate package feerate (CPFP)
- Track mempool eviction state
- Access mempool internal graphs
- Estimate network fees (future: fee estimator module)

### Mempool Subsystem

**Responsibilities**:
- Policy validation (BIP125 RBF, ancestor/descendant limits)
- Package feerate calculation (CPFP)
- Eviction (size limits)
- Transaction expiry
- Conflict detection (double spends)
- Relay decisions (future)

**Does NOT**:
- Handle private keys
- Perform coin selection
- Track wallet balances
- Manage addresses
- Decrypt encrypted wallets
- Make spending decisions

---

## Interface Specification

### Header File (FROZEN)

**Location**: `include/wallet/mempool_interface.h`

**Stability Guarantee**:
- Interface methods may NOT be removed
- Existing parameters may NOT be changed
- New methods may be ADDED (append-only)
- Struct fields may be ADDED (backward-compatible only)

### Data Structures

#### 1. MempoolInfo (Read-Only State)

```cpp
struct MempoolInfo {
    size_t tx_count;              // Current transaction count
    size_t total_bytes;           // Total mempool size (bytes)
    double min_relay_fee_rate;    // Minimum fee to relay (sat/byte)
};
```

**Contract**:
- ✅ Wallet may query this anytime (read-only)
- ✅ No side effects
- ✅ Fast operation (O(1))
- ❌ Wallet may NOT assume internal mempool structure

#### 2. TxPolicyResult (Pre-Validation)

```cpp
struct TxPolicyResult {
    bool would_accept;                      // Would mempool accept?
    std::string rejection_reason;           // Human-readable reason

    uint32_t ancestor_count;                // Unconfirmed ancestors
    uint32_t descendant_count;              // Unconfirmed descendants
    double effective_feerate;               // CPFP-aware feerate

    bool conflicts_exist;                   // Double-spend detected?
    std::vector<std::string> conflicting_txids;
};
```

**Contract**:
- ✅ Wallet calls BEFORE signing (dry-run validation)
- ✅ No mempool state mutation
- ✅ Deterministic result (same tx → same answer)
- ❌ Does NOT sign transaction
- ❌ Does NOT relay transaction

**Use Case**:
```cpp
// Wallet: Check policy before wasting signing effort
auto policy = mempool->testAcceptTransaction(unsigned_tx);
if (!policy.would_accept) {
    return Error("Won't be accepted: " + policy.rejection_reason);
}
// Now safe to sign and submit
```

#### 3. SubmitResult (Post-Submission)

```cpp
struct SubmitResult {
    enum class Status {
        ACCEPTED,       // Added to mempool
        REJECTED,       // Policy violation
        REPLACED        // RBF: Replaced existing tx
    };

    Status status;
    std::string txid;
    std::string reason;
    std::vector<std::string> replaced_txids;

    uint32_t ancestor_count;
    uint32_t descendant_count;
    double effective_feerate;
};
```

**Contract**:
- ✅ Wallet receives AFTER submission
- ✅ Wallet updates UTXO state based on status
- ✅ Wallet tracks replaced TXIDs (RBF)
- ❌ Wallet does NOT retry on rejection (user decides)

#### 4. SubmitMode (Execution Control)

```cpp
enum class SubmitMode {
    TEST_ONLY,      // Policy test (no sigs, no relay, regtest only)
    BROADCAST       // Full validation + network relay
};
```

**Contract**:
- ✅ `TEST_ONLY`: Enables mempool policy testing without Phase 34
- ✅ `BROADCAST`: Production mode with full validation
- ❌ `TEST_ONLY` never relayed to network
- ❌ `TEST_ONLY` never persisted across restarts

---

## Interface Methods (FROZEN)

### 1. getMempoolInfo()

```cpp
virtual MempoolInfo getMempoolInfo() const = 0;
```

**Contract**:
- **Preconditions**: None
- **Postconditions**: Returns current mempool state
- **Side Effects**: None
- **Thread Safety**: Must be thread-safe (read lock)
- **Performance**: O(1) - no heavy computation

**Wallet Usage**:
```cpp
auto info = mempool->getMempoolInfo();
ui->updateMempoolStats(info.tx_count, info.total_bytes);
```

### 2. testAcceptTransaction()

```cpp
virtual TxPolicyResult testAcceptTransaction(
    const Transaction& tx
) const = 0;
```

**Contract**:
- **Preconditions**: `tx` structurally valid (has inputs/outputs)
- **Postconditions**: Returns policy validation result
- **Side Effects**: None (dry-run only)
- **Thread Safety**: Must be thread-safe (read lock)
- **Performance**: O(N) where N = ancestor count

**Wallet Usage**:
```cpp
auto result = mempool->testAcceptTransaction(unsigned_tx);
if (!result.would_accept) {
    if (result.rejection_reason.find("fee") != std::string::npos) {
        // Suggest higher fee to user
        ui->suggestHigherFee(result.effective_feerate);
    }
}
```

### 3. hasTransaction()

```cpp
virtual bool hasTransaction(const std::string& txid) const = 0;
```

**Contract**:
- **Preconditions**: `txid` is valid hex string
- **Postconditions**: Returns true if tx in mempool
- **Side Effects**: None
- **Thread Safety**: Must be thread-safe (read lock)
- **Performance**: O(1) - hash table lookup

**Wallet Usage**:
```cpp
if (mempool->hasTransaction(txid)) {
    utxo_state = WalletUTXOState::UNCONFIRMED;
} else {
    utxo_state = WalletUTXOState::CONFIRMED;
}
```

### 4. submitTransaction()

```cpp
virtual SubmitResult submitTransaction(
    const Transaction& tx,
    SubmitMode mode
) = 0;
```

**Contract**:
- **Preconditions**:
  - `tx` is fully signed (if `mode == BROADCAST`)
  - `tx` passes structural validation
- **Postconditions**:
  - If `ACCEPTED`: tx added to mempool
  - If `REPLACED`: old txs removed, new tx added
  - If `REJECTED`: no state change
- **Side Effects**:
  - May add transaction to mempool
  - May remove conflicting transactions (RBF)
  - May relay to network (if `mode == BROADCAST`)
- **Thread Safety**: Must be thread-safe (write lock)
- **Performance**: O(N + M) where N = ancestors, M = descendants

**Wallet Usage**:
```cpp
auto result = mempool->submitTransaction(signed_tx, SubmitMode::BROADCAST);

switch (result.status) {
case SubmitResult::Status::ACCEPTED:
    wallet->markUTXOsSpent(inputs);
    ui->showSuccess("Transaction broadcast: " + result.txid);
    break;

case SubmitResult::Status::REPLACED:
    wallet->handleRBF(result.replaced_txids, result.txid);
    ui->showInfo("Transaction replaced previous tx");
    break;

case SubmitResult::Status::REJECTED:
    ui->showError("Rejected: " + result.reason);
    // User can adjust fee and retry
    break;
}
```

---

## Error Handling Contract

### Rejection Reasons (Standardized Strings)

**Policy Violations**:
- `"too-long-mempool-chain (ancestor count: X, limit: 25)"`
- `"too-long-mempool-chain (descendant count: X, limit: 25)"`
- `"mempool full (fee rate too low: X sat/byte)"`
- `"Fee rate too low: X (min: Y)"`

**RBF Violations**:
- `"Double spend rejected (original transaction does not signal RBF)"`
- `"RBF rejected (replacement fee X not higher than original Y)"`
- `"RBF rejected (replacement feerate X not higher than original Y)"`

**Structural Violations**:
- `"Transaction has no inputs"`
- `"Transaction has no outputs"`
- `"Cannot find input UTXO: txid:vout"`

**Contract**:
- ✅ Wallet may parse these strings for UI display
- ✅ Strings are stable across versions (append-only)
- ❌ Wallet may NOT depend on exact string format for logic
- ✅ Use `TxPolicyResult` fields for programmatic decisions

---

## Threading Model

### Mempool Guarantees

1. **Read Methods** (const):
   - `getMempoolInfo()`
   - `testAcceptTransaction()`
   - `hasTransaction()`
   - Acquire shared lock (read lock)
   - Multiple threads may call concurrently

2. **Write Methods**:
   - `submitTransaction()`
   - Acquire unique lock (write lock)
   - Only one writer at a time

3. **Deadlock Prevention**:
   - Mempool never calls back into wallet
   - One-way dependency: Wallet → Mempool
   - No recursive locking

### Wallet Responsibilities

- ✅ Wallet may call mempool methods from any thread
- ✅ Wallet must handle async UI updates
- ❌ Wallet may NOT hold wallet lock while calling mempool

---

## Future-Proofing

### What May Change (Backward-Compatible)

1. **New Fields** (append-only):
   ```cpp
   struct MempoolInfo {
       size_t tx_count;
       size_t total_bytes;
       double min_relay_fee_rate;
       // NEW in v0.13.0+
       size_t package_count;  // OK: append field
   };
   ```

2. **New Methods** (append-only):
   ```cpp
   class IMempoolInterface {
       // ... existing methods ...

       // NEW in v0.13.0+
       virtual PackageFeeEstimate estimatePackageFee(...) = 0;  // OK
   };
   ```

3. **New Enum Values** (append-only):
   ```cpp
   enum class SubmitMode {
       TEST_ONLY,
       BROADCAST,
       PACKAGE_RELAY  // NEW in v0.14.0+ - OK
   };
   ```

### What May NOT Change

1. ❌ Remove existing methods
2. ❌ Change method signatures
3. ❌ Reorder struct fields
4. ❌ Change enum values
5. ❌ Break thread safety guarantees

### Deprecation Process

If a method **must** be removed (v0.13.0+):

1. Mark as `[[deprecated]]` in v0.12.0
2. Keep functional for 1 major version
3. Remove in v0.14.0+ (after migration period)

Example:
```cpp
// v0.12.0
[[deprecated("Use getDetailedMempoolInfo() instead")]]
virtual MempoolInfo getMempoolInfo() const = 0;

// v0.13.0 (both exist)
[[deprecated("Use getDetailedMempoolInfo() instead")]]
virtual MempoolInfo getMempoolInfo() const = 0;
virtual DetailedMempoolInfo getDetailedMempoolInfo() const = 0;

// v0.14.0 (old method removed)
virtual DetailedMempoolInfo getDetailedMempoolInfo() const = 0;
```

---

## Versioning

### Interface Versioning

**Current**: `v0.12.0` (Wallet Feature Complete)

**Version Compatibility**:
- Same MAJOR: Backward compatible
- New MINOR: New methods allowed (append-only)
- New PATCH: Bug fixes only (no API changes)

### Change Log

| Version | Date       | Changes |
|---------|------------|---------|
| v0.12.0 | 2025-12-14 | Initial frozen contract |

---

## Testing Requirements

### Mandatory Tests

1. **Interface Compliance**:
   - Mock implementation of `IMempoolInterface`
   - Wallet tests use mock (no real mempool)

2. **Contract Validation**:
   - Test all rejection reasons
   - Test RBF replacement flow
   - Test CPFP detection

3. **Thread Safety**:
   - Concurrent `testAcceptTransaction()` calls
   - Concurrent read + write operations
   - No deadlocks

4. **Error Handling**:
   - All rejection reasons parseable
   - Wallet handles all `SubmitResult::Status` values

### Integration Tests

1. **End-to-End Workflow**:
   ```cpp
   // Build → Test → Sign → Submit
   auto unsigned_tx = wallet->buildUnsigned(...);
   auto policy = mempool->testAcceptTransaction(unsigned_tx.tx);
   assert(policy.would_accept);
   auto signed_tx = wallet->sign(unsigned_tx.tx, keys);
   auto result = mempool->submitTransaction(signed_tx, SubmitMode::BROADCAST);
   assert(result.status == SubmitResult::Status::ACCEPTED);
   ```

2. **RBF Flow**:
   - Submit tx with low fee
   - Submit replacement with higher fee
   - Verify `REPLACED` status
   - Verify `replaced_txids` correct

3. **Policy Rejection**:
   - Build tx violating ancestor limit
   - Verify `testAcceptTransaction()` predicts rejection
   - Verify `submitTransaction()` actually rejects

---

## Enforcement

### Compile-Time Enforcement

1. **Interface** is abstract (`= 0` methods)
2. **Wallet** depends on `IMempoolInterface*` (DI)
3. **Mempool** implements `IMempoolInterface`

### Runtime Enforcement

1. Wallet unit tests use **mock mempool**
2. Integration tests use **real mempool**
3. If wallet calls undefined method → compile error

### Review Enforcement

1. Any PR modifying `mempool_interface.h` requires:
   - Explicit justification
   - Review by 2+ maintainers
   - Documentation update
   - Migration guide (if breaking)

---

## Contact / Governance

**Questions**: See `GOVERNANCE.md` for protocol change procedures

**Breaking Changes**: Require MAJOR version bump (v1.0.0+)

**This contract is FROZEN for v0.12.0. Treat it like consensus code.**

---

**Signatures** (Conceptual):

- ✅ Wallet Subsystem: Agrees to use interface only
- ✅ Mempool Subsystem: Agrees to implement interface faithfully
- ✅ Architecture Review: Frozen until v0.13.0+

**Effective**: 2025-12-14
**Review Date**: v0.13.0 development (earliest 2026 Q1)
