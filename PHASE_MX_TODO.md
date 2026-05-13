# Phase M.X: Future Technical Debt

## Mempool Graph Traversal Bug

**Issue**: `Mempool::removeTransaction(txid, recursive=true)` causes infinite loop

**Test**: `tests/mempool/test_mempool_expiry.cpp` Test 4 (Recursive removal)

**Symptoms**:
- Hang when removing parent transaction with descendants
- Chain A → B → C, removing A should remove B and C
- Process never completes

**Likely Causes** (requires investigation):
1. Child map mutation during iteration
2. Cycle not marked before recursion  
3. Missing visited-set in recursive walk
4. Parent/child edge inconsistency

**Location**: `src/mempool/mempool.cpp` - `removeTransaction()` method

**Priority**: Medium (non-critical path - recursive removal is optimization)

**Fix Strategy**:
- Audit graph traversal algorithm
- Add visited-set to prevent cycles
- Ensure edge consistency (parent/child bidirectional)
- Add comprehensive graph tests after fix

**Status**: DEFERRED - Not a Phase M.0/M.1 regression, pre-existing untested bug

---

*Created during mempool test stabilization - 2025-12-27*
