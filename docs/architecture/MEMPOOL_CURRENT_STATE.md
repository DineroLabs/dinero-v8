# Mempool Current State - Architectural Analysis

**Date**: 2025-12-26
**Purpose**: Document current mempool implementation gaps and proper fix plan

---

## Executive Summary

**Status**: 🔴 **MEMPOOL USES WRONG UTXO STRUCT**

The mempool implementation has a critical architectural issue: it uses a simple `UTXO` struct that doesn't properly exist in the consensus namespace, mixing old wallet-style types with consensus validation.

**Impact**:
- Type confusion between wallet UTXO tracking and consensus validation
- Uses `std::string txid` instead of `uint256`
- Not using canonical ChainStateView abstraction
- Acknowledged in code as "intentionally broken until Phase M.0"

---

## The UTXO Struct Confusion

### What Mempool Uses (WRONG)

**Location**: `src/daemon/mempool.cpp:1787-1810`

```cpp
using namespace consensus;  // Line 1787

// Collect all input UTXOs (needed for BIP341 Taproot sighash)
std::vector<UTXO> input_utxos;  // Line 1790 - Uses "UTXO"
input_utxos.reserve(tx.vin.size());

for (size_t i = 0; i < tx.vin.size(); i++) {
    const auto& input = tx.vin[i];

    // Look up UTXO in either confirmed set or mempool
    UTXO utxo;  // Line 1800 - Creates "UTXO" instance
    utxo.txid = input.prevout.txid;  // Assigns uint256 to... what type?
    utxo.vout = input.prevout.vout;

    if (!utxo_view.GetUTXO(utxo.txid, utxo.vout, utxo.spk, utxo.value)) {
        error = "Input UTXO not found";
        return false;
    }

    input_utxos.push_back(utxo);
}
```

**Problem**: Code does `using namespace consensus;` then uses `UTXO`, but **there is NO struct UTXO in namespace consensus**.

---

### What Actually Exists

#### 1. Old Wallet-Style UTXO (WRONG - should not be used)

**Location**: `src/consensus/block_validation.h:14-25` (namespace `dinero`, NOT `consensus`)

```cpp
namespace dinero {

struct UTXO {
    std::string txid;              // ← WRONG: Should be uint256
    uint32_t vout;
    int64_t value;                 // Amount in una
    std::vector<uint8_t> spk;      // scriptPubKey
    int height;                    // Block height where created
    bool is_coinbase;
    std::optional<int> spend_height;
    std::string path;              // ← Wallet-specific, not consensus

    UTXO() : vout(0), value(0), height(0), is_coinbase(false) {}
};

} // namespace dinero
```

**Issues**:
- ❌ Uses `std::string txid` (should be `uint256`)
- ❌ Has wallet-specific field `path` (not consensus data)
- ❌ In wrong namespace (`dinero` not `consensus`)
- ❌ Deprecated - from old code before proper type migration

---

#### 2. Proper Consensus UTXO Entry (CORRECT - should be used)

**Location**: `include/consensus/utxo_entry.h:21-83` (namespace `consensus`)

```cpp
namespace dinero {
namespace consensus {

struct UTXOEntry {
    uint64_t value;                    // ✓ Correct: Value in una
    std::vector<uint8_t> scriptPubKey; // ✓ Correct: Locking script
    uint32_t height;                   // ✓ Correct: Block height
    bool isCoinbase;                   // ✓ Correct: Coinbase flag

    UTXOEntry() : value(0), height(0), isCoinbase(false) {}

    UTXOEntry(uint64_t val, const std::vector<uint8_t>& script,
              uint32_t h, bool coinbase)
        : value(val), scriptPubKey(script), height(h), isCoinbase(coinbase) {}

    // Check if this UTXO is mature (spendable)
    bool isMature(uint32_t current_height) const {
        if (!isCoinbase) return true;
        return (current_height >= height + 100);  // Coinbase maturity
    }

    size_t serializedSize() const { /* ... */ }
};

} // namespace consensus
} // namespace dinero
```

**Correct Design**:
- ✅ No `txid` field (txid is the OutPoint key, not stored in entry)
- ✅ Proper types (`uint64_t value`, not `int64_t`)
- ✅ Consensus-only fields (no wallet cruft)
- ✅ In correct namespace (`consensus`)
- ✅ Matches Bitcoin Core's `Coin` class pattern

---

### How Does Mempool Code Compile?

**Question**: Mempool does `using namespace consensus;` then uses `UTXO`, but `consensus::UTXO` doesn't exist. Why does this compile?

**Precise C++ Answer**:

**File structure**:
```cpp
// src/daemon/mempool.cpp:47
namespace dinero {

    // ... code ...

    // Line 1787
    using namespace consensus;  // For ScriptExecutionContext, ScriptError, etc.

    // Line 1790
    std::vector<UTXO> input_utxos;  // Uses "UTXO"
}
```

**Name lookup resolution**:
1. Code is already inside `namespace dinero` (line 47)
2. `using namespace consensus;` adds consensus names to lookup
3. When compiler sees `UTXO`:
   - First checks `consensus::UTXO` → **does not exist**
   - Then checks current namespace `dinero::UTXO` → **exists** (found in `include/primitives/transaction.h:32`)
4. Resolves to `dinero::UTXO`

**Key Point**: C++ does NOT do "fallback namespace lookup". This compiles because the code is **already in namespace dinero**, so `dinero::UTXO` is visible in the current scope.

**What exists**:
- `dinero::UTXO` defined in `include/primitives/transaction.h:32` (minimal struct for transaction signing)
- `dinero::UTXO` also defined in `src/consensus/block_validation.h:14` (old wallet-style struct)
- Both are in `namespace dinero`, both are visible

**Result**: Code compiles and uses `dinero::UTXO` (wrong type), NOT `consensus::UTXOEntry` (correct type).

**Why This Actually Strengthens the Argument**:
- This is namespace pollution - `dinero::UTXO` leaking into consensus code
- The comment says `using namespace consensus; // For UTXO...` but `consensus::UTXO` doesn't exist
- Code accidentally picks up wrong type from enclosing namespace
- Exactly the kind of type confusion Phase M.1 must eliminate

---

## Phase M.0 TODO Already Documents This

**Location**: `src/daemon/mempool.cpp:19-45`

```cpp
// ============================================================================
// Phase M.0 TODO (IMPLEMENTATION PENDING)
// ============================================================================
//
// This file is intentionally broken until Phase M.0 Step 4c is completed.
// Headers have migrated txid/OutPoint to uint256 (commits 38424da1-296b828a).
//
// Required work (~3-4 hours):
// 1. Replace all std::string txid with uint256
// 2. Replace string-based outpoint tracking with OutPoint struct
// 3. Update method signatures to match headers (uint256 parameters)
// 4. Move all string conversions to RPC/logging boundary only
//
// DO NOT reintroduce string-based identity.
```

**Status**: Acknowledged as broken, deferred to Phase M.0

---

## Proper Fix Plan (User-Provided)

### NOT TODAY - Future Reference

User provided a proper phased approach for when mempool work is done deliberately:

#### Phase M.1 - Mempool Foundation
- **ChainStateView abstraction**
  - No local UTXO structs
  - Read-only access to canonical chainstate
  - Mempool queries UTXO set through abstraction layer

**Goal**: Eliminate struct confusion - mempool doesn't define its own UTXO types

#### Phase M.2 - Policy Layer
- Fee rules (min relay fee, incremental relay fee)
- Size limits (max standard tx size, max mempool size)
- Orphan handling (transactions with missing inputs)

**Goal**: Separate policy (what should relay) from consensus (what is valid)

#### Phase M.3 - Miner Integration
- Deterministic template building (block construction)
- Stable transaction ordering (ancestor set based)
- CPFP (Child Pays For Parent) support

**Goal**: Mining uses mempool efficiently and predictably

#### Phase M.4 - Adversarial Testing
- Fuzz transaction inputs (malformed, edge cases)
- Conflict storms (many conflicting transactions)
- Fee attacks (pinning, sniping, etc.)

**Goal**: Ensure mempool can't be DoS'd or exploited

---

## Current Mempool Validation Flow

### Script Verification (Added in Phase L0.4)

**Location**: `src/daemon/mempool.cpp:1784-1856`

**Status**: ✅ **CORRECT** - Uses proper consensus flags

```cpp
// Phase L0.4: Script verification with consensus flags
using namespace consensus;

// CRITICAL: MUST use SCRIPT_VERIFY_STANDARD (includes covenant flags)
const uint32_t MEMPOOL_FLAGS = SCRIPT_VERIFY_STANDARD;

for (size_t i = 0; i < tx.vin.size(); i++) {
    const TxInput& txin = tx.vin[i];
    const UTXO& utxo = input_utxos[i];  // ← Uses wrong UTXO struct

    // Create execution context with consensus flags
    ScriptExecutionContext ctx(
        &tx,
        static_cast<uint32_t>(i),
        utxo.value,  // ← Field exists on both UTXO structs
        MEMPOOL_FLAGS
    );
    ctx.all_amounts = all_amounts;
    ctx.all_scriptpubkeys = all_scriptpubkeys;

    // Verify script
    ScriptError script_error;
    if (!VerifyScript(
        Script(txin.scriptSig),
        Script(utxo.spk),  // ← Field exists on both UTXO structs
        txin.witness,
        ctx,
        script_error
    )) {
        error = "Script verification failed";
        return false;
    }
}
```

**Why It Works Despite Wrong Struct**:
- Both `dinero::UTXO` and `consensus::UTXOEntry` have `value` and `spk` fields
- Script verification only accesses those fields
- Code accidentally works, but uses wrong type

**Why This Is Still Bad**:
- Type confusion (which UTXO struct am I using?)
- `dinero::UTXO` has `std::string txid` field that shouldn't exist
- Not following canonical chainstate access pattern
- Fragile - breaks if struct fields diverge

---

## What Phase M.1 Should Look Like

### Correct Pattern: ChainStateView Abstraction

```cpp
// Example: How mempool SHOULD access UTXO data

class ChainStateView {
public:
    // Query UTXO by OutPoint (txid, vout)
    virtual std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const = 0;

    // Check if UTXO exists and is unspent
    virtual bool IsUnspent(const OutPoint& outpoint) const = 0;

    // Get current chain tip height
    virtual uint32_t GetHeight() const = 0;
};

// Mempool uses abstraction, never defines own UTXO struct
bool Mempool::validateTransaction(const Transaction& tx, std::string& error) {
    using namespace consensus;

    // Access UTXO data through abstraction
    for (size_t i = 0; i < tx.vin.size(); i++) {
        const auto& input = tx.vin[i];
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};

        // Get UTXO entry from canonical chainstate
        auto utxo_opt = chainstate_view_->GetUTXO(outpoint);
        if (!utxo_opt.has_value()) {
            error = "Input UTXO not found";
            return false;
        }

        const UTXOEntry& utxo = *utxo_opt;  // ← Uses consensus::UTXOEntry

        // Verify script with consensus flags
        ScriptExecutionContext ctx(&tx, i, utxo.value, SCRIPT_VERIFY_STANDARD);
        if (!VerifyScript(input.scriptSig, utxo.scriptPubKey, input.witness, ctx)) {
            error = "Script verification failed";
            return false;
        }
    }

    return true;
}
```

**Key Improvements**:
- ✅ No local UTXO struct definitions
- ✅ Uses `consensus::UTXOEntry` (correct type)
- ✅ Accesses chainstate through abstraction
- ✅ OutPoint uses uint256 txid
- ✅ Read-only access (mempool doesn't modify UTXO set)

---

## Why This Matters

### Current Risk: Type Confusion

| Aspect | Current (WRONG) | Should Be (Phase M.1) |
|--------|-----------------|----------------------|
| **UTXO Struct** | `dinero::UTXO` (old wallet type) | `consensus::UTXOEntry` (consensus type) |
| **Txid Type** | `std::string txid` field | No txid field (OutPoint is the key) |
| **Namespace** | `using namespace consensus;` but uses `dinero::UTXO` | `consensus::UTXOEntry` explicitly |
| **Access Pattern** | Direct struct manipulation | ChainStateView abstraction |
| **Coupling** | Mempool knows about UTXO internals | Mempool uses read-only interface |

### Impact of Wrong Struct

1. **Type Safety**: Mixing wallet types with consensus types
2. **Correctness**: `std::string txid` vs `uint256` confusion
3. **Maintainability**: Which UTXO struct is being used?
4. **Architecture**: Violates Layer 0 ↔ Mempool boundary

---

## Recommended Action

### Today: Document Only (per user instruction)

✅ **DONE** - This document

### When Phase M.1 Starts (Future)

1. **Remove `dinero::UTXO` from codebase** (or rename to WalletUTXO)
2. **Implement ChainStateView abstraction**
3. **Update mempool to use `consensus::UTXOEntry`**
4. **Remove all `std::string txid` usage**
5. **Verify with grep**: `grep -n "std::string.*txid" src/daemon/mempool.cpp` should return nothing

---

## Files Involved

### Current (Broken)

| File | Issue | Fix Needed |
|------|-------|------------|
| `src/daemon/mempool.cpp:1787-1810` | Uses wrong UTXO struct | Use `consensus::UTXOEntry` |
| `src/consensus/block_validation.h:14-25` | Defines old `dinero::UTXO` | Remove or rename to WalletUTXO |

### Proper (Phase M.1)

| File | Purpose | Status |
|------|---------|--------|
| `include/consensus/utxo_entry.h` | Defines `consensus::UTXOEntry` | ✅ Exists, correct |
| `include/consensus/outpoint.h` | Defines `OutPoint` with `uint256` | ✅ Exists, correct |
| ChainStateView interface (future) | Abstraction for UTXO access | ⏳ Not yet implemented |

---

## Comparison: Wrong vs Right

### Current (Wrong)

```cpp
using namespace consensus;  // Brings in consensus types
std::vector<UTXO> input_utxos;  // ← But UTXO is from dinero namespace!

UTXO utxo;
utxo.txid = input.prevout.txid;  // Assigns uint256 to std::string field
utxo.vout = input.prevout.vout;
```

**Problem**: Type confusion, wrong namespace

### Correct (Phase M.1)

```cpp
using namespace consensus;
std::vector<UTXOEntry> input_utxos;  // ✓ Correct consensus type

OutPoint outpoint{input.prevout.txid, input.prevout.vout};  // ✓ uint256
auto utxo_opt = chainstate_view_->GetUTXO(outpoint);
if (!utxo_opt.has_value()) { /* error */ }
const UTXOEntry& utxo = *utxo_opt;  // ✓ consensus::UTXOEntry
```

**Solution**: Proper types, canonical access pattern

---

## Conclusion

**Current State**: 🔴 Mempool uses wrong UTXO struct (acknowledged as broken)

**Impact**: Type confusion between `dinero::UTXO` (old wallet type) and `consensus::UTXOEntry` (correct consensus type)

**Proper Fix**: Phase M.1 (ChainStateView abstraction) - NOT TODAY

**Why Document This**: User is correct - "we left out mempool" - it needs architectural cleanup but with proper phased approach, not rushed fixes

**Next Steps**: When Phase M starts, follow user's 4-phase plan (M.1 through M.4) for deliberate, correct implementation

---

**Document Date**: 2025-12-26
**Status**: Analysis complete, deferred to Phase M
**Related**: Phase M.0 TODO in `src/daemon/mempool.cpp:19-45`

---

## Addendum: C++ Name Lookup Verification

**Question Resolved**: "How does mempool code compile when using UTXO from consensus namespace that doesn't exist?"

**Answer**: ✅ **Namespace pollution - code is in `namespace dinero`, picks up `dinero::UTXO` from enclosing scope**

**Verified By**:
- `src/daemon/mempool.cpp:47` - File is in `namespace dinero`
- `src/daemon/mempool.cpp:1787` - Does `using namespace consensus;` locally
- `include/primitives/transaction.h:32` - Defines `dinero::UTXO` (minimal struct)
- `src/consensus/block_validation.h:14` - Also defines `dinero::UTXO` (old wallet struct)

**C++ Name Lookup**:
1. Compiler looks for `UTXO` in `consensus::` → not found
2. Compiler looks in current namespace `dinero::` → **found**
3. Resolves to `dinero::UTXO` (wrong type)

**Key Insight**: This is NOT "fallback lookup" - C++ doesn't do that. This is standard name resolution within the current namespace where the code is written.

**Why This Matters**: The namespace pollution makes the architectural problem even clearer - consensus validation code is accidentally using wallet-layer types.

---

**Analysis Validated By**: User C++ clarification (2025-12-26)
**Conclusion**: All architectural findings correct, C++ mechanics now precisely documented
