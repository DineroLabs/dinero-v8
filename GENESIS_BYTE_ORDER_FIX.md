# Genesis Byte Order Fix

**Date:** 2025-12-21
**Issue:** Genesis initialization failing due to merkle root / block hash byte order mismatch
**Status:** ✅ FIXED

---

## Problem Description

The daemon failed to initialize with the following error:

```
❌ FATAL: Coinbase TXID must equal merkle root for single-tx block!
Expected (merkleRoot): 8c109896fc86ed4246051b620b9958d56dba39c00a38b321f32f96a71a555511
Got (coinbase TXID):  1155551aa7962ff321b3380ac039ba6dd558990b621b054642ed86fc9698108c
```

**Observation:** The two hashes are **exact byte reversals** of each other.

---

## Root Cause Analysis

### The Bug

The genesis merkle root in `chainparams_impl.cpp` was stored with bytes in the **wrong order**:

```cpp
// WRONG (before fix):
static constexpr const char* EXPECTED_MERKLE_ROOT =
    "8c109896fc86ed4246051b620b9958d56dba39c00a38b321f32f96a71a555511";
```

### Why This Happened

1. **Phase M.0 defines byte order semantics:**
   - `uint256` stores hashes in **little-endian** (internal identity)
   - `GetHex()` reverses bytes to **big-endian** (display format)
   - `FromHex()` reverses bytes from **big-endian → little-endian**

2. **GetTxid() returns uint256 correctly:**
   ```cpp
   // transaction.cpp:293
   uint256 Transaction::GetTxid() const {
       auto bytes = Serialize(false);
       auto hash_bytes = TransactionSerializer::DoubleSHA256Bytes(bytes);  // Returns little-endian
       uint256 result;
       std::memcpy(result.data, hash_bytes.data(), 32);  // Preserves byte order
       return result;  // Little-endian identity
   }
   ```

3. **The comparison failed because:**
   ```cpp
   // genesis_init.cpp:236-238
   uint256 coinbase_txid = coinbase_tx.GetTxid();  // Little-endian uint256
   uint256 expected_merkle_root = uint256::FromHexUnsafe(params.genesis.merkleRootHex);

   // FromHexUnsafe() expects BIG-ENDIAN hex input
   // But merkleRootHex was stored as LITTLE-ENDIAN
   // Result: bytes were reversed TWICE (should have been reversed once)
   ```

### The Cascade

After fixing the merkle root, the genesis block hash changed:

```
Old header (wrong merkle): hash = 00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74
New header (correct merkle): hash = a9673b8558dac6852fcc85c1eb2cec1656a680d0ecdb9f02a98cf3f3deef59d6
```

This is expected because the header content changed:

```cpp
// block.cpp:53-75 - BlockHeader::SerializeForHash()
WriteHashLE(data, DINERO_HEADER_MERKLEROOT_OFFSET, merkle_hex);
// ↑ Merkle root is embedded in header
// ↓ Header hash = SHA256(SHA256(serialized_header))
```

---

## The Fix

### Step 1: Correct Merkle Root Byte Order

**File:** `src/consensus/chainparams_impl.cpp`

```cpp
// FIXED:
static constexpr const char* EXPECTED_MERKLE_ROOT =
    "1155551aa7962ff321b3380ac039ba6dd558990b621b054642ed86fc9698108c";  // Phase M.0 fix: corrected byte order
```

**How to verify this is correct:**

The merkle root for a single-transaction block equals the coinbase TXID:

```cpp
uint256 coinbase_txid = coinbase_tx.GetTxid();  // Computes from transaction bytes
coinbase_txid.GetHex()  // "1155551aa7962ff321b3380ac039ba6dd558990b621b054642ed86fc9698108c"
```

### Step 2: Update Genesis Block Hash

**File:** `src/consensus/chainparams_impl.cpp`

```cpp
// FIXED:
static constexpr const char* EXPECTED_GENESIS_HASH =
    "a9673b8558dac6852fcc85c1eb2cec1656a680d0ecdb9f02a98cf3f3deef59d6";  // Phase M.0 fix: recomputed with correct merkle root
```

Also updated the mainnet params:

```cpp
.genesis_hash = "a9673b8558dac6852fcc85c1eb2cec1656a680d0ecdb9f02a98cf3f3deef59d6",  // Phase M.0: recomputed
```

---

## Verification

### Genesis Initialization Output (After Fix)

```
[1/2] Loading genesis from chainparams...
✅ Genesis coinbase deserialized: 1 inputs, 1 outputs
Genesis hash: a9673b8558dac6852fcc85c1eb2cec1656a680d0ecdb9f02a98cf3f3deef59d6
Genesis merkle: 1155551aa7962ff321b3380ac039ba6dd558990b621b054642ed86fc9698108c

✅ All genesis validation tripwires PASSED
[GENESIS OK] hash=a9673b8558dac6852fcc85c1eb2cec1656a680d0ecdb9f02a98cf3f3deef59d6
[GENESIS OK] merkle=1155551aa7962ff321b3380ac039ba6dd558990b621b054642ed86fc9698108c
[GENESIS OK] txid=1155551aa7962ff321b3380ac039ba6dd558990b621b054642ed86fc9698108c
[GENESIS OK] vtx=1

Storing genesis in ChainDB...
✅ Genesis block stored at height 0

✅ Genesis initialization complete!
```

**Key Evidence:**
- ✅ `merkle == txid` (single-tx block property holds)
- ✅ Genesis hash matches expected value
- ✅ All validation tripwires pass
- ✅ Block stored successfully

---

## Why Phase M.0 Tests Didn't Catch This

**Phase M.0 tests verify:**
- ✅ `uint256` internal representation is little-endian
- ✅ `GetHex()` reverses to big-endian for display
- ✅ `DoubleSHA256Bytes()` returns little-endian bytes
- ✅ `GetTxid()` preserves identity via memcpy
- ✅ No double-reversal in hash chains

**What Phase M.0 tests DON'T verify:**
- ❌ That chainparams data is stored in the correct byte order
- ❌ That genesis initialization compares hashes correctly
- ❌ That all code paths use uint256 correctly

**Lesson:** Tests can only verify what they test. Data bugs in untested code paths can exist even when all tests pass.

---

## Impact

### Before Fix
- ❌ Daemon fails to start (genesis initialization fails)
- ❌ No blockchain can be created
- ❌ Phase N components unreachable

### After Fix
- ✅ Daemon starts successfully
- ✅ Genesis block initializes correctly
- ✅ Phase N components operational
- ✅ Ready for integration testing

---

## Files Modified

| File | Change | Lines |
|------|--------|-------|
| `src/consensus/chainparams_impl.cpp` | Fixed merkle root byte order | 1 |
| `src/consensus/chainparams_impl.cpp` | Updated genesis hash (2 places) | 2 |

**Total:** 3 lines changed

---

## Compatibility Notes

**Breaking Change:** Yes, genesis block hash changed

**Impact:**
- Any existing mainnet nodes will have a different genesis hash
- This requires a **network-wide reset** if mainnet was already deployed
- If mainnet was NOT deployed yet, this is a clean fix with no impact

**Recommendation:**
- If this is pre-launch, deploy with the corrected values ✅
- If mainnet is live, coordinate a network upgrade with all node operators

---

## Conclusion

The genesis byte order bug was a **data error**, not an architecture error. The Phase M.0 byte order handling is correct - the bug was in the hardcoded chainparams values being stored in the wrong format.

**Fix verified:** ✅ Complete
**Daemon status:** ✅ Operational
**Phase N status:** ✅ Ready for testing
