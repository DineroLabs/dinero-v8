# Mempool Policy Documentation

**Status**: ✅ FROZEN (STEP 3 COMPLETE)
**Version**: v0.11.0
**Last Updated**: 2025-12-14

## Overview

DineroCoin's mempool implements Bitcoin Core-compatible policy rules for transaction acceptance, eviction, and fee bumping. All policies are enforced in `TEST_ONLY` mode to enable comprehensive testing without requiring Phase 34 (transaction signing).

## Policy Rules Implemented

### 1. Basic Acceptance (STEP 3.1)
**Commit**: Initial implementation
**Purpose**: Structural validation and fee calculation

- ✅ Transaction must have inputs and outputs
- ✅ Inputs must exist (mempool UTXO overlay or ChainDB)
- ✅ Fee calculated: `input_value - output_value > 0`
- ✅ Minimum fee rate: 1 sat/byte

### 2. Ancestor Limits (STEP 3.2)
**Commit**: 5940e9b2
**Purpose**: Prevent deep unconfirmed chains

- ✅ **Limit**: 25 transactions max (including self)
- ✅ **Check**: BFS traversal through parent dependencies
- ✅ **Rejection**: `too-long-mempool-chain (ancestor count: 26 including self, limit: 25)`

**Rationale**: Prevents unbounded ancestor traversal during block construction.

### 3. Descendant Limits (STEP 3.3)
**Commit**: 2a5730a1
**Purpose**: Prevent excessive child chains

- ✅ **Limit**: 25 descendants max (including parent)
- ✅ **Size limit**: 101 KB total descendant size
- ✅ **Check**: BFS traversal through children for each parent
- ✅ **Rejection**: Triggers if any parent would exceed limits

**Rationale**: Prevents transaction pinning attacks via descendant trees.

### 4. Mempool Size Limits (STEP 3.4)
**Commit**: 94088f0c
**Purpose**: Bounded memory usage

- ✅ **Limit**: 300 MB (matching Bitcoin Core)
- ✅ **Eviction**: Lowest package feerate evicted first
- ✅ **Rejection**: If new transaction has lowest feerate, reject instead of evicting

**Critical Bug Fixes**:
- Deadlock prevention: Calculate size inline (no recursive lock)
- Safe iterator-erase pattern: `it = erase(it)` prevents invalidation

### 5. Transaction Expiry (STEP 3.5)
**Commit**: f7bd53f4
**Purpose**: Time-based hygiene

- ✅ **Default**: 336 hours (2 weeks)
- ✅ **Execution**: Runs opportunistically before all policy checks
- ✅ **Ordering**: Expiry → size checks → eviction

**Rationale**: Prevents stale low-fee transactions from occupying mempool indefinitely.

### 6. Package Feerate / CPFP (STEP 3.6)
**Commit**: c902275b
**Purpose**: Child-Pays-For-Parent fee bumping

- ✅ **Calculation**: `ancestor_feerate = (sum ancestor fees) / (sum ancestor sizes)`
- ✅ **Eviction**: Uses package feerate, not individual feerate
- ✅ **Benefit**: High-fee children protect low-fee parents

**Fields Added to `MempoolEntry`**:
```cpp
uint64_t ancestor_fee;      // Total fee of tx + all ancestors
size_t ancestor_size;       // Total size of tx + all ancestors
double ancestor_feerate;    // ancestor_fee / ancestor_size
```

**Rationale**: Enables users to bump parent fees by adding high-fee children.

### 7. Replace-By-Fee / RBF (STEP 3.7)
**Commit**: 1036d10d
**Purpose**: Transaction replacement with pinning protections

**BIP125 Rules Enforced**:
1. ✅ **RBF Signaling**: Original transaction must signal RBF (`nSequence < 0xfffffffe`)
2. ✅ **Higher Absolute Fee**: `replacement.fee > original.fee`
3. ✅ **Higher Feerate**: `replacement.feerate > original.feerate`
4. ✅ **Bandwidth Payment**: `replacement.fee >= original.fee + (1 sat/byte * replacement.size)`
5. ✅ **Anti-DoS**: Max 100 transactions replaced

**Pinning Protections**:
- Rule 1: Cannot replace non-RBF transactions
- Rule 4: Prevents tiny fee bumps (must pay for bandwidth)
- Rule 5: Prevents mass eviction DoS
- Ancestor/descendant limits apply to replacement

**Rationale**: Enables fee bumping while preventing abuse.

## Fee Bumping Methods

DineroCoin supports two fee bumping mechanisms:

### CPFP (Child-Pays-For-Parent)
- **How**: Add a child transaction with high fee
- **Effect**: Boosts package feerate, protects parent from eviction
- **Use case**: Cannot modify original transaction (e.g., received from others)

### RBF (Replace-By-Fee)
- **How**: Replace transaction with higher-fee version
- **Effect**: Original transaction removed, replacement added
- **Use case**: Own transaction, want to increase fee
- **Requirement**: Original must signal RBF

## Implementation Details

### Thread Safety
- All operations protected by `m_mutex` (shared or unique lock)
- Size calculations done inline to prevent deadlock
- Safe iterator-erase pattern used throughout

### Data Structures
```cpp
std::unordered_map<std::string, MempoolEntry> m_transactions;  // txid -> entry
std::unordered_set<std::string> m_spent_outputs;               // outpoint tracking
std::multimap<double, std::string> m_fee_index;                // feerate -> txid
std::multimap<TimePoint, std::string> m_time_index;            // time -> txid
```

### Eviction Order
1. **Expiry**: Remove transactions older than `m_max_age` (2 weeks)
2. **Size check**: Calculate total mempool size
3. **Eviction**: If over limit, remove lowest package feerate transactions
4. **Rejection**: If new transaction would be evicted, reject it

## Configuration

All policies use Bitcoin Core defaults:

```cpp
DEFAULT_MAX_SIZE = 300 MB
DEFAULT_MAX_AGE = 336 hours (2 weeks)
DEFAULT_MIN_FEE_RATE = 1.0 sat/byte
MAX_ANCESTOR_COUNT = 24 (25 including self)
MAX_DESCENDANT_COUNT = 24 (25 including parent)
MAX_DESCENDANT_SIZE = 101 KB
INCREMENTAL_RELAY_FEE_RATE = 1.0 sat/byte
MAX_REPLACEMENT_COUNT = 100 transactions
```

## Testing

All policies are tested in `TEST_ONLY` mode:
- Bypasses signature validation (enables testing without Phase 34)
- Full policy enforcement (same rules as production)
- Never relayed to network
- Only available in regtest mode

### Test Coverage
- ✅ Ancestor limits: 25 tx chain accepted, 26th rejected
- ✅ Descendant limits: Linear chain rejection
- ✅ Size limits: No eviction for small tests
- ✅ Expiry: No removal for fresh transactions
- ✅ CPFP: Package feerate calculated correctly
- ✅ RBF: Conflict detection and BIP125 validation

## Future Work (NOT PLANNED)

Mempool policy is **FROZEN**. Do not add features unless fixing bugs.

Potential future enhancements (if ever needed):
- Package relay (BIP 331)
- Full-RBF vs opt-in RBF debate
- Dynamic mempool sizing
- v3 transaction topology rules
- Ephemeral anchors

**Policy**: These are out of scope. Mempool is now boring infrastructure.

## References

- [Bitcoin Core Mempool Policy](https://github.com/bitcoin/bitcoin/blob/master/doc/policy/mempool-replacements.md)
- [BIP125: Replace-By-Fee](https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki)
- [Bitcoin Core CPFP Implementation](https://github.com/bitcoin/bitcoin/pull/7600)

## Maintenance Policy

**Stability First**:
- ✅ Mempool policy is feature-complete
- ✅ Only bug fixes allowed
- ✅ No new features without exceptional justification
- ✅ Bitcoin Core compatibility is the gold standard

**If you need to modify mempool policy:**
1. Ask: "Is this a bug or a feature?"
2. If bug: Fix it with tests
3. If feature: Reconsider whether it's truly necessary
4. Consult Bitcoin Core behavior for compatibility

---

**Status**: This document describes a frozen, stable policy layer.
**Contact**: See GOVERNANCE.md for protocol change procedures.
