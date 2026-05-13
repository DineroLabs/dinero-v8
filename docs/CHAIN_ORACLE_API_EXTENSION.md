# Chain Oracle API Extension (Phase 7 Support)

**Date:** 2026-01-15
**Purpose:** Add transaction query methods to IChainOracle for Phase 7B/7C/7D

---

## Overview

Extended `IChainOracle` interface with transaction query methods required by:
- **Phase 7B.2**: HTLC sweep confirmation tracking
- **Phase 7C**: Justice transaction building (fetch revoked commitment)
- **Phase 7D**: HTLC justice (fetch revoked commitment with HTLCs)

---

## New API Methods

### 1. `getTransaction(txid)` - Transaction Retrieval

```cpp
/**
 * Get transaction by txid (from confirmed blocks).
 * RPC equivalent: getrawtransaction <txid> 1
 * Returns: Transaction hex if found, nullopt if not found
 *
 * Phase 7C/7D: Used by justice oracle to fetch revoked commitment TX
 */
virtual std::optional<std::string> getTransaction(const std::string& txid) const = 0;
```

**Usage:**
- **Justice Oracle**: Fetch revoked commitment transaction to identify claimable outputs
- Queries confirmed blocks (not mempool)
- Returns transaction as hex string

**Example:**
```cpp
auto tx_hex = chain_oracle->getTransaction("abc123...");
if (tx_hex.has_value()) {
    // Parse transaction and identify outputs
}
```

### 2. `getTransactionHeight(txid)` - Confirmation Tracking

```cpp
/**
 * Get block height where transaction was confirmed.
 * RPC equivalent: getrawtransaction <txid> 1 → blockheight field
 * Returns: Block height if confirmed, nullopt if unconfirmed
 *
 * Phase 7B/7C/7D: Used to check sweep/justice confirmation
 */
virtual std::optional<uint64_t> getTransactionHeight(const std::string& txid) const = 0;
```

**Usage:**
- **HTLC Sweep Oracle**: Check if sweep transaction confirmed
- **Justice Oracle**: Check if justice transaction confirmed
- Returns block height (not depth)

**Example:**
```cpp
auto height = chain_oracle->getTransactionHeight("def456...");
if (height.has_value() && current_height >= height.value() + 6) {
    // Transaction has 6 confirmations
}
```

---

## Implementation Status

### `MockChainOracle` ✅ Complete

Added test configuration methods:
```cpp
void setTransaction(const std::string& txid, const std::string& tx_hex, uint64_t height) {
    m_transactions[txid] = tx_hex;
    m_tx_heights[txid] = height;
}
```

Implementations:
```cpp
std::optional<std::string> getTransaction(const std::string& txid) const override {
    auto it = m_transactions.find(txid);
    if (it != m_transactions.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<uint64_t> getTransactionHeight(const std::string& txid) const override {
    auto it = m_tx_heights.find(txid);
    if (it != m_tx_heights.end()) {
        return it->second;
    }
    return std::nullopt;
}
```

**Status**: Fully functional for unit tests

### `ProductionChainOracle` ⏳ Conservative Placeholders

Current implementation:
```cpp
std::optional<std::string> ProductionChainOracle::getTransaction(const std::string& txid) const {
    // TODO Phase 7C/7D: Implement transaction retrieval from chainstate
    // Real implementation would:
    // 1. Parse txid from hex string
    // 2. Query ChainDB for transaction
    // 3. Serialize transaction to hex
    // 4. Return transaction hex
    (void)txid;  // Suppress warning
    return std::nullopt;
}

std::optional<uint64_t> ProductionChainOracle::getTransactionHeight(const std::string& txid) const {
    // TODO Phase 7B/7C/7D: Implement confirmation height query
    // Real implementation would:
    // 1. Parse txid from hex string
    // 2. Query ChainDB for transaction confirmation height
    // 3. Return block height if confirmed
    (void)txid;  // Suppress warning
    return std::nullopt;
}
```

**Status**: Compiles successfully, returns conservative `std::nullopt`

**Blocked By**: ChainDB API for transaction queries (not yet exposed)

---

## Required ChainDB Methods (Not Yet Available)

To complete `ProductionChainOracle` implementation, we need:

### 1. Transaction Retrieval
```cpp
// In ChainDB or ChainstateService:
std::optional<Transaction> getTransaction(const uint256& txid) const;
```

### 2. Confirmation Height Query
```cpp
// In ChainDB or ChainstateService:
std::optional<uint64_t> getTransactionHeight(const uint256& txid) const;
```

These methods likely need to:
- Query block index to find transaction
- Access block data to retrieve transaction
- Return confirmation height from block index

---

## Architecture Benefits

### L1/L2 Separation
- **Before**: Oracles accessed `DaemonContext::chainstate` directly
- **After**: Oracles use `IChainOracle` interface
- **Benefit**: Clear architectural boundary, testable with mocks

### Test Isolation
- Unit tests can use `MockChainOracle` without running full node
- Test scenarios easily configured with `setTransaction()`
- Deterministic test behavior

### Future RPC Support
- Interface designed to map cleanly to RPC calls
- `getrawtransaction <txid> 1` → `getTransaction()`
- Easy to implement RPC-based oracle for multi-process architecture

---

## Testing Strategy

### Unit Tests (MockChainOracle)
```cpp
TEST(JusticeOracleTest, BuildJusticeTxWithMockChain) {
    // Setup
    MockChainOracle chain_oracle;
    chain_oracle.setTransaction(
        "revoked_commit_txid",
        "0200000001...",  // TX hex
        1000  // Confirmed at height 1000
    );

    // Test
    auto justice_tx = oracle->buildJusticeTransaction(justice, channel);
    ASSERT_TRUE(justice_tx.isOk());
}
```

### Integration Tests (ProductionChainOracle)
Once ChainDB API is available:
```cpp
TEST(JusticeOracleIntegrationTest, BuildJusticeTxFromRealChain) {
    // Requires real chainstate with confirmed transactions
    ProductionChainOracle chain_oracle(daemon_ctx);

    // Mine revoked commitment to chain
    // ...

    auto justice_tx = oracle->buildJusticeTransaction(justice, channel);
    ASSERT_TRUE(justice_tx.isOk());
}
```

---

## Files Modified

### Interface Definition
- **`include/lightning/chain_oracle.h`**
  - Added `getTransaction()` to `IChainOracle`
  - Added `getTransactionHeight()` to `IChainOracle`
  - Extended `MockChainOracle` with test methods
  - Added storage for mock transactions/heights

### Production Implementation
- **`include/lightning/production_chain_oracle.h`**
  - Added method declarations
- **`src/lightning/production_chain_oracle.cpp`**
  - Added conservative placeholder implementations
  - Returns `std::nullopt` until ChainDB API available

### No Breaking Changes
- Existing methods unchanged
- Backward compatible
- All tests compile and pass

---

## Build Status

```bash
cmake --build build --target dinero_lightning
# Output: [100%] Built target dinero_lightning
```

✅ **All targets compile successfully**

---

## Next Steps

### Immediate (To Unlock Phase 7)
1. **ChainDB Transaction Query**:
   - Add `getTransaction(txid)` to ChainDB or ChainstateService
   - Query block index + block data
   - Serialize transaction to hex

2. **ChainDB Height Query**:
   - Add `getTransactionHeight(txid)` to ChainDB
   - Query block index for confirmation height
   - Return height from CBlockIndex

3. **ProductionChainOracle Integration**:
   - Replace `std::nullopt` placeholders with real queries
   - Handle edge cases (tx not found, mempool vs confirmed)

### Testing
1. Add ChainDB mocks if needed
2. Test transaction retrieval edge cases
3. Test height queries for unconfirmed TXs

---

## Summary

Extended `IChainOracle` with transaction query methods required for Phase 7B/7C/7D. Mock implementation is complete and functional for unit tests. Production implementation has conservative placeholders that return `std::nullopt` until ChainDB API is available.

This completes the **interface architecture** for Phase 7 chain queries. The remaining work is **ChainDB integration** (straightforward API addition).

---

*Architecture designed for clean L1/L2 separation with testable interfaces and RPC-compatible design.*
