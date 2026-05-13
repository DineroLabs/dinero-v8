# Mempool Hardening - v0.11.0

**Date**: December 13, 2025
**Phase**: Policy Layer Hardening
**Status**: 🔧 **IN PROGRESS**

---

## Executive Summary

The consensus layer is **frozen and validated** (v0.10.0 milestone). This release focuses on **mempool policy hardening** - protecting the transaction relay layer from DoS attacks while maintaining protocol-grade discipline.

**Critical Distinction**:
- **Consensus** (frozen): Rules that determine valid blocks
- **Policy** (this release): Rules that determine mempool acceptance

**Invariant**: Every policy rule must be bypassable by a valid block.

---

## What Was Fixed (Bugs)

### 1. Orphan Container Bug (tx_mempool.cpp:182)

**Problem**: Code referenced `orphan_peers_.clear()` but header defined `orphan_children_`
**Root Cause**: Header/implementation mismatch (zombie code)
**Fix**: Replaced `orphan_peers_.clear()` → `orphan_children_.clear()`
**Impact**: Orphan pool now clears correctly

```cpp
// Before (BROKEN):
orphan_peers_.clear();  // Member doesn't exist!

// After (FIXED):
orphan_children_.clear();  // Correct member
```

### 2. RBF Placeholder Logic (mempool_policy.cpp:120-129)

**Problem**: `MempoolPolicy::checkRBF()` had hardcoded placeholder fees
**Root Cause**: Stub code never finished, never called
**Fix**: Removed stub (use `RBFPolicy` for BIP125 validation)
**Impact**: No actual impact (code was dead), but removed confusion

```cpp
// Removed stub with placeholder fees:
uint64_t existing_fee_rate = 1000; // Placeholder
uint64_t new_fee_rate = 1250; // Placeholder
```

---

## What Exists (Strong Foundation)

### RBF Policy (BIP125-Compliant)

**File**: `src/policy/rbf_policy.cpp`
**Status**: ✅ **Complete and correct**

All 5 BIP125 rules implemented:

1. **Rule #1**: Signal replacement (sequence < 0xfffffffe)
2. **Rule #2**: No new unconfirmed inputs
3. **Rule #3**: Higher absolute fee
4. **Rule #4**: Pays for bandwidth (fee delta >= eviction cost)
5. **Rule #5**: Max 100 replacements

**Conflict set building**: Tracks direct conflicts + descendants
**Fee delta calculation**: Properly calculates incremental relay fee

### Standardness Policy

**File**: `src/policy/mempool_policy.cpp`
**Status**: ✅ **Bitcoin-compatible**

- Script type validation (P2PKH, P2SH, P2WPKH, P2WSH, P2TR, OP_RETURN)
- BIP141-aware size/weight limits
- Dust detection (546 una threshold)
- Version checks (v1 and v2 transactions)

### Core Mempool

**File**: `src/daemon/tx_mempool.cpp`
**Status**: ✅ **Functional baseline**

- Entry management (Add/Remove/Exists)
- Fee rate indexing (multiset for O(log n) retrieval)
- Size tracking with eviction hook
- Orphan pool with metadata (peer_id, time_added, depth)

---

## What Needs Hardening (Gaps)

### Priority 1: Missing Implementations

1. **Ancestor/Descendant Tracking**
   - Methods declared in header but not implemented
   - CPFP package limits not enforced
   - Impact: No protection against package-based pinning attacks

2. **Orphan Timeout Eviction**
   - Orphans never expire (20-minute timeout not enforced)
   - No depth limit enforcement (10-level max)
   - Impact: Orphan pool can grow unbounded

3. **Eviction Policy**
   - Basic scoring exists but not tuned
   - No anti-pinning heuristics
   - Impact: Low-fee txs with many descendants may not evict cleanly

### Priority 2: DoS Hardening

4. **Rate Limiting**
   - No per-peer failed transaction limits
   - No orphan accumulation limits per peer
   - Impact: Single peer can spam mempool

5. **Package Validation**
   - Ancestor/descendant size limits not checked
   - Chain depth limits not enforced
   - Impact: CPFP chains can bypass fee requirements

### Priority 3: Testing

6. **Stress Test Suite**
   - Framework created: `tests/test_mempool_stress.sh`
   - 7 test categories defined
   - **Current status**: 1/7 implemented, 0/7 passing

7. **Attack Scenarios**
   - RBF pinning attack
   - CPFP bloat attack
   - Orphan flooding
   - Fee sniping

---

## Stress Test Status

**File**: `tests/test_mempool_stress.sh`
**Framework**: ✅ Created
**Implementation**: ⚠️ 1/7 tests implemented

### Test Categories

1. **Mempool Size Limits** - ⚠️ Not implemented
2. **Fee-Based Eviction** - ⚠️ Not implemented
3. **RBF Pinning Resistance** - ⚠️ Not implemented
4. **Orphan Pool Limits** - ⚠️ Not implemented
5. **Package Limits (CPFP)** - ⚠️ Not implemented
6. **DoS Protection** - ⚠️ Not implemented
7. **Policy/Consensus Boundary** - ⚠️ Not implemented

### Why Tests Aren't Implemented Yet

**Blocker**: Need transaction creation/signing infrastructure for regtest

Options:
- Use `dinero-cli createrawtransaction` + `signrawtransaction`
- Implement Python test harness (like Bitcoin Core)
- Use wallet RPC methods (if available)

**This is the correct approach**: Write failing tests first, then harden.

---

## Policy Invariants (To Be Proven)

These are **policy rules**, not consensus rules. They guide mempool acceptance but **cannot override consensus**.

### Invariant 1: Eviction Preserves Highest Fee Density

**Rule**: When mempool is full, lowest-fee-rate transactions are evicted first

**Test**: Fill mempool with high-fee txs, submit low-fee tx, verify eviction
**Status**: ⚠️ Not tested

### Invariant 2: RBF Prevents Pinning (BIP125 Rule #5)

**Rule**: No more than 100 original transactions can be replaced

**Test**: Create pinning attack scenario with 101 descendants
**Status**: ⚠️ Not tested

### Invariant 3: Orphans Expire After Timeout

**Rule**: Orphans older than 20 minutes are evicted

**Test**: Submit orphan, wait 20 minutes, verify eviction
**Status**: ⚠️ Not implemented (no timeout enforcement)

### Invariant 4: Package Limits Enforced (BIP125)

**Rule**: Max 25 ancestors, max 25 descendants, max 101KB each

**Test**: Create 26-ancestor chain, verify rejection
**Status**: ⚠️ Not implemented (no limit enforcement)

### Invariant 5: Policy Rejection ≠ Consensus Rejection

**Rule**: Mempool can reject tx for policy, but block with that tx MUST be accepted

**Test**: Create low-fee tx, verify mempool rejects, mine block with it, verify block accepted
**Status**: ⚠️ Not tested

---

## Hardening Roadmap

### Phase 1: Core Implementations (v0.11.0)

✅ Fix orphan container bug
✅ Remove RBF placeholder logic
✅ Create stress test framework
⬜ Implement ancestor/descendant tracking
⬜ Add orphan timeout eviction
⬜ Harden eviction policy scoring

### Phase 2: DoS Protection (v0.11.1)

⬜ Per-peer rate limiting
⬜ Orphan accumulation limits
⬜ Package size validation
⬜ Chain depth enforcement

### Phase 3: Testing & Validation (v0.11.2)

⬜ Implement all 7 stress tests
⬜ RBF attack scenarios
⬜ CPFP bloat resistance
⬜ Policy/consensus boundary proof

### Phase 4: Performance (v0.11.3)

⬜ Mempool persistence (optional)
⬜ Fee estimation improvements
⬜ Compact block relay preparation

---

## Critical Distinctions

### What This Is NOT

❌ **Not consensus changes** - Consensus is frozen (v0.10.0)
❌ **Not performance optimization** - This is correctness
❌ **Not feature expansion** - This is hardening existing code
❌ **Not mempool persistence** - Optional, non-critical

### What This IS

✅ **Policy hardening** - DoS resistance
✅ **BIP125 compliance** - Bitcoin-compatible RBF
✅ **Test-driven validation** - Prove correctness
✅ **Attack resistance** - Pinning, flooding, bloat

---

## Policy vs Consensus Examples

### Example 1: Low-Fee Transaction

**Policy**: Mempool rejects tx with fee < 1000 sat/kB (MinRelayFee)
**Consensus**: Block containing that tx is **ACCEPTED** (fee is not a consensus rule)

**Test**: Create 1-una-fee tx, verify mempool rejects, mine block with it, verify block accepted

### Example 2: Dust Output

**Policy**: Mempool rejects tx creating 545-una output (below dust threshold)
**Consensus**: Block containing that tx is **ACCEPTED** (dust is not a consensus rule)

### Example 3: RBF Signal Missing

**Policy**: Mempool rejects replacement if original doesn't signal RBF (BIP125 Rule #1)
**Consensus**: Block containing replacement is **ACCEPTED** (RBF is not a consensus rule)

### Example 4: Non-Standard Script

**Policy**: Mempool rejects OP_CAT script (non-standard opcode)
**Consensus**: Block containing that tx is **ACCEPTED** (if script validates correctly)

---

## Next Steps

1. **Implement ancestor/descendant tracking** (Priority 1)
2. **Add orphan timeout enforcement** (Priority 1)
3. **Create transaction signing helper** (enables stress tests)
4. **Implement 7 stress tests** (validation)
5. **Harden based on test failures** (iterative)

---

## Success Criteria (v0.11.0 Release)

✅ All stress tests passing
✅ RBF attack scenarios handled correctly
✅ Orphan pool limits enforced
✅ Package (CPFP) limits enforced
✅ Policy/consensus boundary proven
✅ No new consensus changes (frozen layer untouched)

**When complete**: Mempool is DoS-resistant and Bitcoin-compatible.

---

## References

- **BIP125**: Opt-in Replace-By-Fee (RBF)
- **BIP141**: Segregated Witness (size/weight validation)
- **Bitcoin Core**: `src/policy/rbf.cpp`, `src/txmempool.cpp`
- **Consensus freeze**: `CONSENSUS_VALIDATION_MILESTONE.md`

---

**Status**: v0.11.0 in progress
**Next milestone**: All stress tests passing
