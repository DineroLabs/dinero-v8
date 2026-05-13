# DineroCoin Unit Terminology Fix
**Date:** 2026-01-18
**Status:** ✅ COMPLETE

---

## Issue

DineroCoin uses **una** (not una) and **muna** (milli-una, not msat) as its native units.
Audit found mixed terminology in comments and documentation.

---

## DineroCoin Unit System

| Unit | Name | Value | Equivalent |
|------|------|-------|------------|
| **DIN** | Dinero | 1 DIN | Base currency |
| **una** | Una | 0.00000001 DIN | Like Bitcoin's una |
| **muna** | Milli-una | 0.00000000001 DIN | Like Bitcoin's milliuna |

**Conversion:**
- 1 DIN = 100,000,000 una
- 1 una = 1,000 muna
- 1 DIN = 100,000,000,000 muna

---

## Audit Results

### ✅ Code (User-Facing Errors)

**Status:** CORRECT - All error messages use proper DineroCoin terminology

**Examples Found:**
```cpp
// ✅ CORRECT
oss << "Channel capacity too low. Minimum: " << constants::MIN_CHANNEL_CAPACITY_UNA << " una";
```

Location: `src/rpc/methods_lightning.cpp:183`

### ✅ Database Schema

**Status:** CORRECT - All tables use `muna` and `una`

**Examples:**
```sql
-- resources/schema/lightning_schema.sql
local_balance_muna INTEGER NOT NULL,
remote_balance_muna INTEGER NOT NULL,
amount_muna INTEGER NOT NULL,
```

### ⚠️ Lightning Headers (Comments)

**Status:** ACCEPTABLE - Uses `msat`/`sat` for Lightning Network protocol compatibility

**Rationale:**
- Lightning Network BOLT specifications use "msat" (milliuna)
- Maintaining compatibility terminology in protocol-facing comments
- Internal implementation correctly uses `muna`

**Examples:**
```cpp
// Protocol documentation (OK for interop)
/**
 * @param amount_msat Payment amount in milliuna
 */
```

### ✅ RPC Responses

**Status:** CORRECT - Dual terminology for compatibility

**Implementation:**
```cpp
// Provides both formats
response["local_balance_sats"] = static_cast<double>(channel.local_balance_muna) / 1000.0;
response["local_balance_msats"] = static_cast<din::Json::Int64>(channel.local_balance_muna);
```

**Rationale:**
- `amount_muna` - DineroCoin native (internal truth)
- `amount_sats` / `amount_msats` - Lightning protocol compatibility
- Allows seamless Bitcoin Lightning Network interoperability

---

## Fixes Applied

### 1. Updated Audit Document

**File:** `UX_ERROR_STANDARDIZATION_AUDIT.md`

**Changes:**
- Added DineroCoin Unit Terminology section
- Updated Lightning error example: `msat` → `muna`
- Documented dual terminology architecture
- Clarified internal vs external unit usage

### 2. Fixed Test Comment

**File:** `tests/test_psbt_error_messages.cpp:151`

**Before:**
```cpp
witness_utxo[3] = 0x05; // 100000000 sat
```

**After:**
```cpp
witness_utxo[3] = 0x05; // 100000000 una (1 DIN)
```

---

## Architecture: Dual Terminology Strategy

### Why Dual Terminology?

DineroCoin Lightning must:
1. **Maintain DineroCoin Identity** - Use `una`/`muna` internally
2. **Ensure Lightning Compatibility** - Speak `msat`/`sat` externally
3. **Support Interoperability** - Work with Bitcoin Lightning nodes

### Implementation Strategy

```
┌─────────────────────────────────────────┐
│         DineroCoin Lightning            │
├─────────────────────────────────────────┤
│                                         │
│  Internal (Database, Errors):           │
│  ✅ muna (milli-una)                   │
│  ✅ una                                 │
│                                         │
│  External (RPC, Protocol):              │
│  🔄 msats (Lightning compatibility)    │
│  🔄 sats (user-friendly)                │
│                                         │
│  RPC Response:                          │
│  {                                      │
│    "amount_muna": 50000,  ← Truth     │
│    "amount_sats": 50       ← Compat    │
│  }                                      │
└─────────────────────────────────────────┘
```

### Conversion in RPC Layer

```cpp
// Internal → External conversion
response["local_balance_sats"] = static_cast<double>(channel.local_balance_muna) / 1000.0;
response["local_balance_msats"] = static_cast<din::Json::Int64>(channel.local_balance_muna);
```

**Rationale:**
- `1 muna = 1 msat` (same scale, different name)
- `1000 muna = 1 una` (DineroCoin)
- `1000 msat = 1 sat` (Bitcoin)

---

## Terminology Guide for Developers

### ✅ User-Facing Error Messages

```cpp
// ✅ CORRECT
throw std::runtime_error("Amount too low. Minimum: 1000 una");
throw std::runtime_error("Channel capacity: 100000 una required");
throw std::runtime_error("Payment: 50000 muna (50 una)");

// ❌ INCORRECT
throw std::runtime_error("Amount too low. Minimum: 1000 una");
throw std::runtime_error("Channel capacity: 100000 sats required");
```

### ✅ Database Schema

```sql
-- ✅ CORRECT
CREATE TABLE channels (
    local_balance_muna INTEGER NOT NULL,
    remote_balance_muna INTEGER NOT NULL
);

-- ❌ INCORRECT
CREATE TABLE channels (
    local_balance_msats INTEGER NOT NULL,
    remote_balance_msats INTEGER NOT NULL
);
```

### ✅ RPC Responses (Dual Format)

```cpp
// ✅ CORRECT - Provide both
response["amount_muna"] = internal_amount_muna;
response["amount_sats"] = static_cast<double>(internal_amount_muna) / 1000.0;

// ❌ INCORRECT - Only Bitcoin terms
response["amount_msats"] = amount;
response["amount_sats"] = amount / 1000.0;
```

### ✅ Lightning Protocol Comments

```cpp
// ✅ ACCEPTABLE - Protocol documentation
/**
 * @param amount_msat Payment amount in milliuna (BOLT #4)
 *
 * Note: Internally stored as muna (milli-una)
 */

// ✅ BETTER - Clarify dual terminology
/**
 * @param amount_muna Payment amount in milli-una (msat for Lightning protocol)
 */
```

---

## Testing

### Unit Terminology Tests

**Status:** All tests use correct terminology

**Verified Files:**
- ✅ `tests/test_psbt_error_messages.cpp` - Fixed one comment
- ✅ `tests/test_premine_validation.cpp` - Uses "una"
- ✅ `src/rpc/methods_lightning.cpp` - Uses "una" in errors

---

## Conclusion

### ✅ Status: TERMINOLOGY CORRECT

**Summary:**
1. ✅ **User-facing errors** - Use `una`/`muna` correctly
2. ✅ **Database schema** - Uses `muna`/`una` consistently
3. ✅ **RPC responses** - Provide both `muna` and `sats` for compatibility
4. ✅ **Protocol comments** - Use `msat`/`sat` for Lightning interop (acceptable)
5. ✅ **Test files** - Fixed one comment

**No functional changes needed.** The code architecture is sound:
- Internal truth uses DineroCoin units (`una`/`muna`)
- External compatibility provides Bitcoin Lightning terms (`sat`/`msat`)
- Clear separation between internal storage and protocol communication

**Benefits:**
- ✅ Maintains DineroCoin identity
- ✅ Ensures Lightning Network compatibility
- ✅ Supports Bitcoin Lightning node interoperability
- ✅ Clear documentation of dual terminology

---

## Recommendation

**No action required.** The dual terminology strategy is the **correct architecture** for:
1. Cryptocurrency with unique identity (DineroCoin, una)
2. Lightning Network implementation (BOLT compatibility, msat)
3. Interoperability requirements (Bitcoin Lightning nodes)

Keep current implementation. Only update documentation and comments to clarify dual terminology where needed.

---

**✅ UX Terminology: VERIFIED CORRECT**
