# Utreexo Consensus Sealing

This document describes the structural safeguards that prevent Utreexo consensus bypasses.

## Overview

In DineroCoin, Utreexo is **consensus-critical from genesis**. The 128-byte header includes the Utreexo commitment, and all nodes must validate it from block 0.

This is NOT optional. There is no "UTXO-only" mode, no shadow mode, no gradual rollout.

## Structural Guarantees

### 1. No Bypass Flag

There is no `strict_utreexo_enforcement_` flag. The flag was **removed entirely**.

```cpp
// BEFORE (vulnerable):
if (strict_utreexo_enforcement_) {
    // reject
} else {
    // shadow mode - accept anyway (DANGEROUS)
}

// AFTER (sealed):
error = "bad-utreexo-root (ROOT_MISMATCH)";
return false;  // ALWAYS reject, no condition
```

### 2. Forest Required at Active Heights

At the **top** of `ConnectBlockInternal()`:

```cpp
if (IsUtreexoActive(height) && !utreexo_forest_) {
    std::cerr << "❌ [FATAL] Utreexo forest is NULL at active height" << std::endl;
    std::abort();
}
```

This runs **before any validation logic**. A missing forest at an active height is impossible to bypass.

### 3. Root Mismatch = Always Reject

```cpp
// Root mismatch handling (no conditions, no flags):
error = "bad-utreexo-root (ROOT_MISMATCH)";
return false;
```

## What Was Removed

| Item | Status |
|------|--------|
| `strict_utreexo_enforcement_` member | **REMOVED** |
| `setStrictUtreexoEnforcement()` method | **REMOVED** |
| `getStrictUtreexoEnforcement()` method | **REMOVED** |
| Shadow mode code path | **REMOVED** |
| `DINERO_TESTING` compile guard | **NOT NEEDED** |

## Invariants (Structurally Enforced)

1. **Utreexo validation cannot be disabled**
   - No flag exists to disable it
   - No API to disable it
   - No config option to disable it

2. **Forest must exist at active heights**
   - Check is at the TOP of validation (first thing)
   - Missing forest = `std::abort()`

3. **Root mismatch = rejection**
   - No shadow mode
   - No logging-only path
   - No "continue anyway" option

## Attack Surface Elimination

| Attack Vector | Status |
|--------------|--------|
| Disable via API | **IMPOSSIBLE** (API removed) |
| Disable via config | **IMPOSSIBLE** (no config) |
| Disable via compile flag | **IMPOSSIBLE** (no flag) |
| Skip via null forest | **IMPOSSIBLE** (abort at top) |
| Accept bad root | **IMPOSSIBLE** (always rejects) |

## Verification

```bash
# Search for any bypass code (should return nothing):
grep -r "strict_utreexo\|shadow.*mode\|bypass.*utreexo" src/ include/

# Verify abort is present:
grep -A2 "IsUtreexoActive.*utreexo_forest_" src/consensus/block_validation.cpp
```

## Summary

The consensus sealing approach is **Option A: Remove shadow mode entirely**.

- No flags
- No conditions
- No bypasses
- Always validate
- Always reject mismatches
- Abort on missing forest

This is governance-by-invariant at the code level.
