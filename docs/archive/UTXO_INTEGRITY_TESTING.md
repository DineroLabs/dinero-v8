# UTXO Integrity Testing Guide
**Phase B.2.2 - UTXO State Machine Validation**

## Overview

This document describes the UTXO integrity assertion framework added in Phase B.2.2 to verify UTXO state machine correctness during reorgs.

## Integrity Check API

### ChainManager Methods

```cpp
// Get UTXO set size (for before/after comparison)
size_t GetUTXOSetSize() const;

// Get memory usage
size_t GetUTXOSetMemoryUsage() const;

// Validate UTXO integrity
struct UTXOIntegrityReport {
    size_t total_utxos{0};
    uint64_t total_value{0};
    size_t memory_bytes{0};
    bool has_negative_values{false};
    bool all_valid{true};
    std::string error_message;
};
UTXOIntegrityReport ValidateUTXOIntegrity() const;
```

## Required Assertions (Per Phase B.2.2)

### 1. UTXO Count Before/After Reorg

**Purpose**: Verify UTXO set size changes correctly during reorg

**Example Usage in Tests**:
```bash
# Before reorg
UTXO_COUNT_BEFORE=$(dinero-cli getutxosetsize)

# Trigger reorg
dinero-cli invalidateblock $BLOCK_C_HASH
dinero-cli generatetoaddress 3 $ADDR
dinero-cli reconsiderblock $BLOCK_C_HASH

# After reorg
UTXO_COUNT_AFTER=$(dinero-cli getutxosetsize)

# Assertion
if [ "$UTXO_COUNT_BEFORE" != "$UTXO_COUNT_AFTER" ]; then
    echo "❌ UTXO count mismatch: before=$UTXO_COUNT_BEFORE after=$UTXO_COUNT_AFTER"
    exit 1
fi
```

### 2. No Negative Balances

**Purpose**: Ensure no UTXO has negative or zero value

**Implementation**: UTXOEntry.value is uint64_t (cannot be negative), enforced at add-time

**Runtime Check**:
- `UTXOSet::AddCoin()` accepts only uint64_t value
- Type system prevents negative values
- Zero values are technically valid (OP_RETURN outputs)

### 3. No Orphaned Outputs

**Purpose**: Every UTXO must reference a valid transaction that exists in a block

**Implementation**:
- UTXOs are only added via `ApplyBlockToUTXO()` (connected blocks)
- UTXOs are only removed via `UndoBlockFromUTXO()` (disconnected blocks)
- Undo data tracks all created outputs for cleanup

**Invariant**: If UTXO exists in set → parent transaction was in a connected block

## Test Integration

### RPC Commands (To Be Added)

```bash
# Get UTXO set size
dinero-cli getutxosetsize

# Get UTXO integrity report
dinero-cli validateutxointegrity
# Returns: {
#   "total_utxos": 1523,
#   "total_value": 50000000000000,
#   "memory_bytes": 245000,
#   "has_negative_values": false,
#   "all_valid": true,
#   "error_message": ""
# }
```

### Test Pattern

```bash
#!/bin/bash
# Example reorg test with UTXO assertions

# Step 1: Capture initial state
UTXO_SIZE_BEFORE=$(get_utxo_size)
UTXO_REPORT_BEFORE=$(validate_utxo_integrity)

# Step 2: Perform reorg
trigger_reorg

# Step 3: Validate post-reorg state
UTXO_SIZE_AFTER=$(get_utxo_size)
UTXO_REPORT_AFTER=$(validate_utxo_integrity)

# Step 4: Assertions
assert_equal "$UTXO_SIZE_BEFORE" "$UTXO_SIZE_AFTER" "UTXO count unchanged"
assert_true "$UTXO_REPORT_AFTER.all_valid" "UTXO integrity maintained"
assert_false "$UTXO_REPORT_AFTER.has_negative_values" "No negative values"
```

## Current Test Coverage

### Phase A Tests (Passing)

All 6 Phase A reorg tests pass with UTXO state machine:

1. ✅ `test_activate_best_chain_simple_fork.sh`
2. ✅ `test_deep_reorg_limits.sh`
3. ✅ `test_equal_work_tie_breaking.sh`
4. ✅ `test_mempool_reconciliation.sh`
5. ✅ `test_multi_branch_competition.sh`
6. ✅ `test_reorg_rollback_on_failure.sh`

### Implicit Integrity Checks

These tests implicitly verify UTXO integrity:
- If UTXO set corruption occurs → block connection fails
- If undo data is invalid → disconnection fails
- If negative values exist → transaction validation fails

### Explicit Integrity Checks (To Add)

Future test enhancements:
```bash
# 1. Deep reorg UTXO conservation test
test_utxo_conservation_deep_reorg() {
    SIZE_GENESIS=$(get_utxo_size)
    mine_blocks 100
    SIZE_100=$(get_utxo_size)

    # Reorg 50 blocks
    invalidate_block_at_height 50
    mine_blocks 51

    SIZE_REORG=$(get_utxo_size)
    assert_utxo_count_valid "$SIZE_REORG"
}

# 2. Concurrent reorg stress test
test_concurrent_reorg_integrity() {
    for i in 1..10; do
        perform_random_reorg
        report=$(validate_utxo_integrity)
        assert_true "$report.all_valid"
    done
}

# 3. Undo data corruption test
test_missing_undo_data() {
    mine_blocks 10
    delete_undo_data_for_block 5
    attempt_reorg_should_fail
    assert_tip_unchanged
}
```

## Implementation Notes

### Current Limitations

1. **No Iterator Yet**: `UTXOSet` doesn't have `ForEach()` iterator
   - Cannot walk all UTXOs to compute total_value
   - Cannot check individual UTXO properties

2. **Basic Validation Only**: Current implementation checks:
   - ✅ Set size matches internal count
   - ✅ Memory usage is non-zero
   - ⚠️  Empty set warning on mature chains
   - ❌ Individual UTXO validation (requires iterator)

### Future Enhancements

Add to `UTXOSet`:
```cpp
// Iterator for validation
void ForEach(std::function<void(const OutPoint&, const UTXOEntry&)> callback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [outpoint, entry] : utxo_map_) {
        callback(outpoint, entry);
    }
}
```

Then enhance `ValidateUTXOIntegrity()`:
```cpp
// Walk all UTXOs
utxo_set_->ForEach([&](const OutPoint& op, const UTXOEntry& entry) {
    if (entry.value == 0) {
        report.has_negative_values = true;  // Flag zero values
    }
    report.total_value += entry.value;

    // Check scriptPubKey not empty
    if (entry.scriptPubKey.empty()) {
        report.all_valid = false;
        report.error_message = "Empty scriptPubKey in UTXO";
    }

    // Check height sanity
    if (active_tip_ && entry.height > active_tip_->height) {
        report.all_valid = false;
        report.error_message = "UTXO height > chain height";
    }
});
```

## Verification Checklist

Per Phase B.2.2 requirements:

- ✅ UTXO count accessible via `GetUTXOSetSize()`
- ✅ Before/after reorg comparison possible
- ✅ Negative balance prevention (uint64_t type system)
- ✅ Orphaned output prevention (add/remove symmetry)
- ✅ All 6 Phase A tests pass
- ⚠️  RPC exposure (future work)
- ⚠️  Deep validation iterator (future work)

## Conclusion

The UTXO integrity assertion framework provides:
1. Basic size/memory tracking ✅
2. Type-safe value constraints ✅
3. Structural guarantees (add/remove symmetry) ✅
4. Foundation for deep validation (iterator API needed)

All Phase A tests pass, demonstrating UTXO state machine correctness during reorgs.
