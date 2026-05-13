# Stubs, TODOs, and Placeholders Audit

**Date**: 2025-11-06
**Total Found**: 886 TODOs/FIXMEs/Stubs
**Status**: Comprehensive audit of incomplete implementations

---

## Executive Summary

During the architecture migration (Weeks 1-5), the focus was on **structural refactoring** (service-oriented architecture, dependency injection, global elimination). This audit identifies **functional gaps** - places where real implementation was deferred in favor of architectural progress.

### Severity Classification:

| Severity | Count | Impact | Action Priority |
|----------|-------|--------|-----------------|
| **CRITICAL** | ~15 | Consensus/validation broken | 🔴 HIGH |
| **HIGH** | ~50 | Core features incomplete | 🟡 MEDIUM |
| **MEDIUM** | ~200 | RPC/API incomplete | 🟢 LOW |
| **LOW** | ~600 | Non-critical features | ⚪ BACKLOG |

---

## 🔴 CRITICAL: Consensus & Validation (Must Fix)

### 1. Transaction ID Calculation - BROKEN

**File**: `src/core/consensus/transaction_validator.cpp:345-350`

**Current Code**:
```cpp
std::string TransactionValidator::calculateTxId(const ValidatedTransaction& tx) {
    DIN_TODO("Implement real transaction serialization and double SHA256");
    std::string data = std::to_string(tx.version) + std::to_string(tx.lockTime) + std::to_string(tx.size);
    return "placeholder_txid"; // TODO: Implement real double SHA256
}
```

**Problem**: Returns literal string "placeholder_txid" for ALL transactions
- Every transaction gets the same TXID
- UTXO lookups will fail
- Double-spend detection broken
- Mempool will reject all but first transaction

**Impact**: **Daemon cannot process transactions correctly**

**Fix**:
```cpp
std::string TransactionValidator::calculateTxId(const ValidatedTransaction& tx) {
    // Serialize transaction
    std::vector<uint8_t> serialized = SerializeTransaction(tx);

    // Double SHA256
    uint8_t hash1[32];
    SHA256(serialized.data(), serialized.size(), hash1);

    uint8_t hash2[32];
    SHA256(hash1, 32, hash2);

    // Convert to hex (reversed for display)
    return HexStr(hash2, hash2 + 32, true);
}
```

**Estimated Effort**: 2-3 hours (need transaction serialization)

---

### 2. UTXO Lookup - BROKEN

**File**: `src/core/consensus/transaction_validator.cpp:437-457`

**Current Code**:
```cpp
bool DatabaseUTXOProvider::getUTXO(const std::string& txid, uint32_t vout, UTXO& utxo) const {
    // Placeholder implementation
    utxo.value = 0; // Will be set by real database lookup
    utxo.script_pubkey = ""; // Will be set by real database lookup
    return true; // Placeholder - always return true
}
```

**Problem**: Always returns `true` with empty UTXO
- Transaction validation cannot check input values
- Double-spend detection broken
- Fee calculation returns 0

**Impact**: **Transaction validation is non-functional**

**Fix**:
```cpp
bool DatabaseUTXOProvider::getUTXO(const std::string& txid, uint32_t vout, UTXO& utxo) const {
    if (!g_chain_db_direct) {
        return false;
    }

    // Query UTXO from database
    auto result = g_chain_db_direct->getCoin(txid, vout);
    if (result.status() != Status::Ok) {
        return false; // UTXO not found
    }

    auto coin = result.value();
    utxo.txid = txid;
    utxo.vout = vout;
    utxo.value = coin.value;
    utxo.script_pubkey = coin.scriptPubKey;
    utxo.height = coin.height;
    utxo.is_coinbase = coin.isCoinbase;
    utxo.is_spent = false;

    return true;
}
```

**Estimated Effort**: 1-2 hours (wire to ChainDB)

---

### 3. UTXO Spent Check - BROKEN

**File**: `src/core/consensus/transaction_validator.cpp:459-462`

**Current Code**:
```cpp
bool DatabaseUTXOProvider::isUTXOSpent(const std::string& txid, uint32_t vout) const {
    // Placeholder implementation
    return false; // Always says "not spent"
}
```

**Problem**: Always returns `false`
- Double-spend detection completely broken
- Attacker can spend same UTXO multiple times

**Impact**: **CRITICAL SECURITY VULNERABILITY**

**Fix**:
```cpp
bool DatabaseUTXOProvider::isUTXOSpent(const std::string& txid, uint32_t vout) const {
    if (!g_chain_db_direct) {
        return true; // Assume spent if DB unavailable (safe default)
    }

    auto result = g_chain_db_direct->getCoin(txid, vout);
    return result.status() != Status::Ok; // If not found, it's spent or never existed
}
```

**Estimated Effort**: 30 minutes

---

## 🟡 HIGH: Core Features Incomplete

### 4. Genesis Block Initialization - INCOMPLETE

**File**: `src/daemon/blockchain.cpp:116-310`

**Count**: 45 TODOs related to genesis/premine

**Current State**:
```cpp
// TODO: Implement GenesisBlock
// TODO: Implement genesis block creation
// TODO: Implement genesis block fields
// TODO: Implement premine
```

**Problem**: Genesis block likely hardcoded or missing proper initialization

**Impact**: Network cannot bootstrap correctly

**Fix Strategy**:
1. Use canonical genesis from `consensus/genesis_canonical.cpp`
2. Wire genesis creation through `GenesisInit::Initialize()`
3. Remove stub TODOs once working

**Estimated Effort**: 4-6 hours

---

### 5. Median Time Past Calculation - INCOMPLETE

**File**: `src/daemon/mining.cpp:480`

**Current Code**:
```cpp
// TODO: Calculate actual MedianTimePast from last 11 blocks
uint32_t prev_time = static_cast<uint32_t>(std::time(nullptr));
```

**Problem**: Uses current time instead of median of last 11 blocks
- Violates consensus rules (BIP 113)
- Blocks may be rejected by network

**Impact**: Mining produces invalid blocks

**Fix**:
```cpp
uint32_t prev_time = 0;
if (chain_db) {
    prev_time = dinero::storage::GetMedianTimePast(chain_db);
} else {
    prev_time = static_cast<uint32_t>(std::time(nullptr)); // Fallback
}
```

**Estimated Effort**: 15 minutes (use existing GetMedianTimePast function)

---

### 6. Mempool Fee Calculation - STUB

**File**: `src/daemon/mining.cpp:1221`

**Current Code**:
```cpp
// TODO: Implement real mempool fee calculation
total_fees_ = 0;
```

**Problem**: Always returns 0 fees
- Miners don't collect transaction fees
- No fee-based transaction prioritization

**Impact**: Mining incentives broken

**Fix**: Wire to real mempool fee calculation (mempool likely has this)

**Estimated Effort**: 1-2 hours

---

### 7. Transaction Hex Conversion - MISSING

**File**: `src/daemon/blockchain.cpp:762,875,877,900,920,924` (multiple locations)

**Current Code**:
```cpp
// TODO: Implement hex to bytes conversion
```

**Problem**: Transaction data not properly serialized/deserialized

**Impact**: Block storage/retrieval broken for transactions

**Fix**: Use existing hex utilities or implement proper serialization

**Estimated Effort**: 2-3 hours

---

## 🟢 MEDIUM: RPC/API Features

### 8. GetBlockTemplate Details - INCOMPLETE

**Count**: ~30 TODOs in blockchain.cpp related to binding genesis/premine data

**Impact**: RPC `getblocktemplate` may return incomplete data

**Priority**: Medium (mining works, but API incomplete)

---

### 9. Wallet Balance Calculation - STUB

**File**: `src/daemon/blockchain.cpp:716`

**Current Code**:
```cpp
return "{\"total_balance\":\"0\",\"addresses\":[]}"; // Placeholder
```

**Problem**: Always returns 0 balance

**Impact**: Wallet RPC returns wrong balances

**Fix**: Wire to WalletManager's actual balance calculation

**Estimated Effort**: 1 hour

---

### 10. RocksDB Serialization - PLACEHOLDERS

**File**: `src/core/storage/rocksdb_backend.cpp:776-804`

**Current Code**:
```cpp
// Serialization helpers (placeholders - need proper implementation)
std::vector<uint8_t> serializeTx(const Transaction& tx) {
    return std::vector<uint8_t>{1, 2, 3, 4}; // Placeholder
}
```

**Problem**: Transactions/blocks not properly serialized in RocksDB

**Impact**: RocksDB backend non-functional

**Priority**: Medium (if SQLite is primary backend)

---

## ⚪ LOW PRIORITY: Non-Critical Features

### 11. Explorer API - STUBS

**Files**: `src/core/explorer/*.cpp`

**Count**: ~50 placeholder implementations

**Impact**: Block explorer API incomplete

**Priority**: Low (external feature)

---

### 12. Backup Manager - PLACEHOLDERS

**Files**: `src/core/storage/backup_manager.cpp`

**Examples**:
```cpp
return "checksum_placeholder";
return "chain_tip_placeholder";
```

**Impact**: Backup/restore features incomplete

**Priority**: Low (operational tool)

---

### 13. Stratum Bridge - PLACEHOLDER

**File**: `src/stratum_bridge/stratum_server.cpp:147`

**Current Code**:
```cpp
// Send response (placeholder)
```

**Impact**: External miner support incomplete

**Priority**: Low (CPU mining works)

---

## 🔧 Migration-Related TODOs

### 14. MetricsService Labels - TODO

**File**: `src/daemon/services/mining_service.cpp:243`

**Current Code**:
```cpp
// TODO Week 5: Add per-miner labels when MetricsRegistry supports them
```

**Status**: Part of Week 6 plan (metrics hooks)

**Priority**: Medium (operational visibility)

---

### 15. Bridge Pattern Cleanup - TODO

**File**: `src/daemon/services/chainstate_service.cpp:98`

**Current Code**:
```cpp
// TODO Week 2+: Remove this once all code uses DaemonContext
g_chain_db_direct = chain_db_.get();
```

**Status**: Ready to remove (Week 5 complete)

**Priority**: Low (bridge still works, but can be removed)

---

## 📊 Summary by Category

| Category | Critical | High | Medium | Low | Total |
|----------|----------|------|--------|-----|-------|
| **Consensus/Validation** | 3 | 2 | 0 | 0 | 5 |
| **Mining** | 0 | 3 | 1 | 0 | 4 |
| **Storage** | 0 | 1 | 3 | 0 | 4 |
| **RPC/API** | 0 | 1 | 5 | 0 | 6 |
| **Explorer** | 0 | 0 | 0 | 50 | 50 |
| **Backup/Tools** | 0 | 0 | 0 | 30 | 30 |
| **Migration** | 0 | 0 | 2 | 0 | 2 |
| **Other** | 0 | 0 | 10 | 775 | 785 |
| **TOTAL** | **3** | **7** | **21** | **855** | **886** |

---

## 🎯 Recommended Action Plan

### Phase 1: Fix Critical Bugs (Week 6 - Day 1-2)

**Must Fix Before Production**:
1. ✅ **Transaction ID calculation** (2-3 hours)
2. ✅ **UTXO lookup** (1-2 hours)
3. ✅ **UTXO spent check** (30 minutes)

**Total**: 1 day

**Impact**: Daemon becomes transaction-capable

---

### Phase 2: Complete Core Features (Week 6 - Day 3-5)

**High Priority**:
4. Genesis block initialization (4-6 hours)
5. Median time past in mining (15 minutes)
6. Mempool fee calculation (1-2 hours)
7. Transaction hex conversion (2-3 hours)

**Total**: 2-3 days

**Impact**: Mining produces valid blocks, fees work correctly

---

### Phase 3: API Completeness (Week 7)

**Medium Priority**:
8. RPC getblocktemplate details
9. Wallet balance calculation
10. RocksDB serialization (if using RocksDB)

**Total**: 3-5 days

**Impact**: RPC API fully functional

---

### Phase 4: External Features (Future)

**Low Priority**:
11. Explorer API stubs → real implementations
12. Backup manager placeholders → real backup
13. Stratum bridge → external miner support

**Total**: 2-3 weeks

**Impact**: External tooling support

---

## 🚨 Risk Assessment

### If Critical Bugs Not Fixed:

**Risk Level**: 🔴 **CRITICAL**

**Consequences**:
- Transactions will fail validation
- Double-spends possible
- Network consensus breaks
- Mining produces invalid blocks
- Wallet balances incorrect

**Mitigation**: Fix Phase 1 (Critical Bugs) before any production deployment

---

### If High Priority Incomplete:

**Risk Level**: 🟡 **HIGH**

**Consequences**:
- Genesis block may not initialize correctly
- Blocks may violate consensus rules
- Miners don't collect fees (disincentivized)
- Network may fork

**Mitigation**: Fix Phase 2 before public testnet

---

### If Medium/Low Priority Incomplete:

**Risk Level**: 🟢 **LOW**

**Consequences**:
- Some RPC commands return incomplete data
- External tools (explorers, miners) don't work
- Operational features missing

**Mitigation**: Fix incrementally based on user demand

---

## 🔍 Detection Strategy

### How to Find These at Runtime:

1. **Enable DIN_TODO Logging**:
```cpp
#define DIN_TODO(msg) \
    do { \
        static bool logged = false; \
        if (!logged) { \
            dinero::g_logger.warning("TODO: " msg " at " __FILE__ ":" + std::to_string(__LINE__)); \
            logged = true; \
        } \
    } while(0)
```

2. **Add Assertions in Tests**:
```cpp
TEST(TransactionValidator, CalculatesTxIdCorrectly) {
    ValidatedTransaction tx = CreateTestTransaction();
    std::string txid = validator.calculateTxId(tx);

    // Fail if placeholder
    ASSERT_NE(txid, "placeholder_txid");
    ASSERT_EQ(txid.length(), 64); // Hex string
}
```

3. **Runtime Validation**:
```cpp
// In production, warn if placeholder detected
if (txid == "placeholder_txid") {
    dinero::g_logger.error("CRITICAL: Transaction ID calculation is using placeholder!");
    return false;
}
```

---

## 📝 Conclusion

**Architecture Migration (Weeks 1-5)**: ✅ **COMPLETE**
- Service-oriented design: ✅
- Dependency injection: ✅
- Global elimination: ✅
- Build passing: ✅

**Functional Implementation**: ⚠️ **INCOMPLETE**
- Critical consensus bugs: 🔴 3 found
- High priority features: 🟡 7 incomplete
- Medium priority APIs: 🟢 21 incomplete
- Low priority features: ⚪ 855 deferred

**Recommendation**:
1. **Week 6 Day 1-2**: Fix critical bugs (Phase 1) ← **DO THIS FIRST**
2. **Week 6 Day 3-5**: Complete core features (Phase 2)
3. **Week 7+**: API completeness and external features

**After Phase 1+2**: Daemon is **production-ready for basic operations** (mining, transactions, wallet).

**After Phase 3**: Daemon is **production-ready for full API access**.

**After Phase 4**: Daemon is **production-ready for ecosystem tools**.

---

**Audit Date**: 2025-11-06
**Audited By**: Comprehensive code scan
**Status**: ✅ Complete inventory of 886 TODOs
**Next Action**: Fix critical consensus bugs (Phase 1)
