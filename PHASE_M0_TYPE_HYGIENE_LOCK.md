# Phase M.0 - Type Hygiene LOCK

**Status:** ✅ **LOCKED** (December 19, 2025)

**Purpose:** Enforce uint256-based transaction identity across all mempool code.

---

## Invariants (Non-Negotiable)

### 1. Core Mempool Identity

**RULE:** All core mempool logic MUST use `uint256` for transaction identity.

**Enforcement:**
- `include/daemon/mempool.h` - All methods accept `const uint256&`
- `include/daemon/tx_mempool.h` - All methods accept `const uint256&`
- `include/mempool/mempool.h` - All methods accept `const uint256&`
- `src/daemon/mempool.cpp` - All internal logic uses `uint256`

**Prohibited:**
```cpp
// ❌ NEVER DO THIS:
std::unordered_map<std::string, MempoolEntry> m_transactions;
bool hasTransaction(const std::string& txid);
std::vector<std::string> getTransactionIds();
```

**Required:**
```cpp
// ✅ ALWAYS DO THIS:
std::unordered_map<uint256, MempoolEntry> m_transactions;
bool hasTransaction(const uint256& txid);
std::vector<uint256> getTransactionIds();
```

### 2. Single Canonical OutPoint

**RULE:** Only ONE OutPoint definition exists: `include/consensus/outpoint.h`

**Definition:**
```cpp
struct OutPoint {
    uint256 txid;    // NOT std::string, NOT Hash256
    uint32_t vout;

    bool operator==(const OutPoint& other) const;
    std::string ToString() const;  // RPC boundary ONLY
    static OutPoint FromString(const std::string& str);  // RPC boundary ONLY
};
```

**Enforcement:**
```bash
# This MUST return ZERO results:
grep -r "struct OutPoint" include/ src/ | grep -v consensus/outpoint.h
```

**Prohibited:**
```cpp
// ❌ NEVER define OutPoint anywhere else:
struct OutPoint {
    std::string txid;  // WRONG
    // ...
};

struct OutPoint {
    Hash256 hash;  // WRONG
    // ...
};
```

### 3. No String Concatenation for Identity

**RULE:** NEVER use string concatenation for outpoint identity.

**Prohibited:**
```cpp
// ❌ NEVER DO THIS:
std::string outpoint = txid + ":" + std::to_string(vout);
m_spent_outputs.insert(outpoint);
```

**Required:**
```cpp
// ✅ ALWAYS DO THIS:
OutPoint outpoint{txid, vout};
m_spent_outputs.insert(outpoint);
```

**Enforcement:**
```bash
# This MUST return ZERO results:
grep 'txid.*+.*":".*vout' src/daemon/mempool.cpp
```

### 4. RPC Boundary Conversion

**RULE:** String ↔ uint256 conversion happens ONLY at RPC boundaries.

**Required Pattern:**
```cpp
// ✅ RPC Input:
std::string txid_hex = params["txid"].as<std::string>();
uint256 txid = uint256::FromHexUnsafe(txid_hex);
auto tx = mempool.getTransaction(txid);  // Core uses uint256

// ✅ RPC Output:
result["txid"] = txid.GetHex();  // Convert once at boundary
```

**Prohibited:**
```cpp
// ❌ NEVER convert inside core logic:
auto tx = mempool.getTransaction(txid.GetHex());  // WRONG
```

**RPC Files:**
- `src/rpc/methods_mempool_context.cpp` - Enforces boundary

### 5. Logging Boundary

**RULE:** Logging uses `.GetHex()` at output boundary.

**Required:**
```cpp
// ✅ ALWAYS DO THIS:
MPLOG_INFO("Added transaction: " + txid.GetHex());
MPLOG_DEBUG("Removed " + txid.GetHex() + " from mempool");
```

**Prohibited:**
```cpp
// ❌ NEVER DO THIS:
MPLOG_INFO("Added transaction: " + txid);  // Won't compile
```

---

## Commit History

Phase M.0 was completed in 9 commits:

1. **38424da1** - Step 1: Canonical OutPoint definition
2. **11cd2010** - Step 2: TxMempoolEntry header migration
3. **6f174fd5** - Step 3: MempoolEntry header migration
4. **296b828a** - Step 4a: daemon::Mempool header migration
5. **94a5f941** - Step 4.1: Conflict tracking (OutPoint structs)
6. **d13d1907** - Step 4.2: Method signatures (uint256 params)
7. **27f47c29** - Step 4.3: Internal containers (uint256 keys)
8. **50da212d** - Step 4.4: Logging (GetHex boundaries)
9. **d79cbc45** - Step 7: RPC boundary enforcement
10. **945daaa8** - Step 8: Delete duplicate OutPoints

---

## Verification Commands

**Run these before ANY mempool changes:**

```bash
# 1. No string outpoint concatenation
grep 'txid.*+.*":".*vout' src/daemon/mempool.cpp
# ✅ MUST return: (nothing)

# 2. Only one OutPoint definition
grep -r "struct OutPoint" include/ src/ | grep -v consensus/outpoint.h
# ✅ MUST return: (nothing)

# 3. Core mempool uses uint256
grep "std::string.*txid" include/daemon/mempool.h include/daemon/tx_mempool.h include/mempool/mempool.h src/daemon/mempool.cpp | grep -v "//" | grep -v "GetHex\|Phase M.0\|GetConfidentialUTXO"
# ✅ MUST return: (nothing)
```

---

## Dependencies

**Phase M.1 (Mempool Completion) depends on these invariants.**

Any violation of M.0 invariants will break M.1:
- Economic testing requires accurate fee tracking (uint256 precision)
- RBF logic requires structural equality (OutPoint-based)
- CPFP ancestor tracking requires proper identity (uint256 sets)

**DO NOT bypass M.0 invariants** under time pressure, bug fixes, or feature requests.

---

## Extension Points (Allowed)

The following are explicitly ALLOWED without violating M.0:

### ✅ RPC Helper Methods
```cpp
// Allowed in headers:
std::string GetTxIdHex() const { return txid.GetHex(); }
```

### ✅ Event/Monitoring Systems
Event publishers (e.g., `mempool_events.h`) may use string txid for JSON serialization, but:
- MUST NOT be called from core mempool logic
- MUST convert at event boundary, not in mempool

### ✅ Interface Methods
Legacy interfaces (e.g., `GetConfidentialUTXO(const std::string& txid)`) may exist if:
- Used by external modules not yet migrated
- Conversion happens at interface boundary
- Core mempool still uses uint256 internally

---

## Prohibited Compatibility Shims

**NEVER** add the following under any circumstances:

```cpp
// ❌ FORBIDDEN:
bool hasTransaction(const std::string& txid) {
    return hasTransaction(uint256::FromHexUnsafe(txid));
}

// ❌ FORBIDDEN:
std::string MempoolEntry::GetTxid() const {
    return txid.GetHex();  // Should use GetTxIdHex() for RPC only
}
```

If external code needs migration, migrate it. Do not add shims.

---

## Rationale

### Why uint256?

1. **Structural Equality**: `uint256` has proper `operator==`, enabling correct container lookups
2. **Hashing**: `std::hash<uint256>` is well-defined and efficient
3. **Memory**: 32 bytes (uint256) vs 64 bytes (hex string)
4. **Precision**: No risk of hex parsing errors in hot paths
5. **Bitcoin Compatibility**: Matches Bitcoin Core's internal representation

### Why Single OutPoint?

1. **Type Safety**: Prevents accidental mixing of incompatible OutPoint types
2. **Refactor Safety**: Changes propagate correctly through type system
3. **Performance**: Single hash function implementation, no conversion overhead

### Why No String Concatenation?

1. **Correctness**: String concat (`txid + ":"`) has no structural equality semantics
2. **Performance**: String allocation + concatenation on every comparison
3. **Bugs**: Easy to accidentally compare hex strings with different capitalization

---

## Lock Status

**This invariant set is LOCKED.**

Changes require:
1. Explicit architectural review
2. Updated lock document
3. Migration plan for existing code

**Last Updated:** December 19, 2025
**Next Phase:** M.1 (Mempool Feature Completion)
