# Ring 3 Phase 4d: TS2 Lock Order Mapping

**Date:** 2026-01-02
**Status:** Analysis Complete
**Next Step:** Identify violations and refactor

---

## 1. Lock Inventory

Current P2PManager has **2 mutexes**:

| Lock | Location | Purpose | TS2 Level |
|------|----------|---------|-----------|
| `peers_mutex_` | p2p_manager.h:189 | Guards `connected_peers_` registry | **LEVEL 2** |
| `outbox_mutex_` | p2p_manager.h:167 | Guards `outbox_queue_` and async send queue | **LEVEL 1** |

**Proposed TS2 Hierarchy:**
```
outbox_mutex_ ≺ peers_mutex_
```

**Rationale:**
- Outbox operations may need to iterate over peers to queue messages
- Peer operations should NOT need to access outbox internals
- This allows broadcast to hold outbox lock while acquiring peers lock

---

## 2. Lock Acquisition Analysis

### Single-Lock Acquisitions (TS2 Compliant)

Most operations acquire only one lock:

| Function | Line | Lock | TS2 Compliant |
|----------|------|------|---------------|
| `start()` | 404 | `peers_mutex_` | ✅ Single lock |
| `stop()` | 424, 456 | `peers_mutex_` | ✅ Single lock |
| `accept_loop()` | 535, 559 | `peers_mutex_` | ✅ Single lock |
| `peer_handler_loop()` | 646, 801 | `peers_mutex_` | ✅ Single lock |
| `cleanup_peer()` | 816 | `peers_mutex_` | ✅ Single lock |
| `get_connected_peers()` | 849 | `peers_mutex_` | ✅ Single lock |
| `disconnect_peer()` | 854 | `peers_mutex_` | ✅ Single lock |
| `send_message_to_peer()` | 879, 942 | `peers_mutex_` | ✅ Single lock |
| `add_seed_node()` | 1008 | `peers_mutex_` | ✅ Single lock |
| `async_outbox_handler()` | 1271, 1287, 1314, 1323, 1334 | `outbox_mutex_` or `peers_mutex_` (separate scopes) | ✅ No nesting |
| `process_message()` | 1390, 1401 | `peers_mutex_` | ✅ Single lock |
| `keepalive_loop()` | 1420, 1435 | `peers_mutex_` | ✅ Single lock |

### Nested-Lock Acquisitions (TS2 Critical)

Only **1 location** has nested lock acquisition:

| Function | Lines | Lock Order | TS2 Status |
|----------|-------|------------|------------|
| `broadcast_message_async()` | 1032 → 1041 | `outbox_mutex_` → `peers_mutex_` | ✅ COMPLIANT |

**Code:**
```cpp
void P2PManager::broadcast_message_async(const P2PMessage& message) {
    auto data = std::make_shared<std::vector<uint8_t>>(message.serialize());

    // Acquire outbox_mutex_ first
    std::lock_guard<std::mutex> lock(outbox_mutex_);  // Line 1032

    if (outbox_queue_.size() >= MAX_OUTBOX_SIZE) {
        return;
    }

    // Acquire peers_mutex_ second (while holding outbox_mutex_)
    std::lock_guard<std::mutex> peers_lock(peers_mutex_);  // Line 1041

    for (const auto& pair : connected_peers_) {
        if (pair.second->is_connected) {
            // Queue message for peer
            outbox_queue_.push_back(...);
        }
    }

    outbox_cv_.notify_one();
}
```

**Analysis:**
- Acquires `outbox_mutex_` at line 1032
- Then acquires `peers_mutex_` at line 1041 (while still holding outbox_mutex_)
- Lock order: **outbox_mutex_ → peers_mutex_** ✅
- This matches our proposed TS2 hierarchy

---

## 3. Lock Order Verification

### Checking for TS2 Violations

**Question:** Is there any code path that acquires locks in the order `peers_mutex_` → `outbox_mutex_`?

**Analysis Method:**
1. Find all places where `peers_mutex_` is acquired
2. Check if any of those scopes subsequently acquire `outbox_mutex_`

**Result:**

| Function | Acquires `peers_mutex_` First? | Then Acquires `outbox_mutex_`? | Violation? |
|----------|-------------------------------|-------------------------------|------------|
| `start()` | Yes (line 404) | No | ✅ Safe |
| `stop()` | Yes (lines 424, 456) | No | ✅ Safe |
| `accept_loop()` | Yes (lines 535, 559) | No | ✅ Safe |
| `peer_handler_loop()` | Yes (lines 646, 801) | No | ✅ Safe |
| `cleanup_peer()` | Yes (line 816) | No | ✅ Safe |
| `get_connected_peers()` | Yes (line 849) | No | ✅ Safe |
| `disconnect_peer()` | Yes (line 854) | No | ✅ Safe |
| `send_message_to_peer()` | Yes (lines 879, 942) | No | ✅ Safe |
| `add_seed_node()` | Yes (line 1008) | No | ✅ Safe |
| `process_message()` | Yes (lines 1390, 1401) | No | ✅ Safe |
| `keepalive_loop()` | Yes (lines 1420, 1435) | No | ✅ Safe |

**Conclusion:** No TS2 violations detected!

---

## 4. Manual Unlock Hazard

Found one **manual unlock** pattern:

| Function | Lines | Pattern | TS2 Risk |
|----------|-------|---------|----------|
| `accept_loop()` | 543-545 | Manual unlock/lock for blocking accept() | ⚠️ VIOLATES TS2.3 |

**Code:**
```cpp
// Line 535: Acquire peers_mutex_
std::lock_guard<std::mutex> lock(peers_mutex_);

while (!shutdown_requested_) {
    // Line 543: MANUAL UNLOCK to avoid blocking under lock
    peers_mutex_.unlock();  // ← TS2 FORBIDS THIS
    int peer_fd = accept(listen_fd_, ...);  // Blocking I/O
    peers_mutex_.lock();   // ← Reacquire

    // ... handle connection ...
}
```

**TS2 Violation:**
- **TS2.3:** "No blocking operations occur while holding any mutex"
- This pattern manually unlocks to perform blocking `accept()`, then reacquires
- While it prevents deadlock, it violates TS2's "no manual unlock" principle
- This is brittle: if exception is thrown, lock state is corrupted

**Fix Required:** Refactor to avoid manual unlock/lock pattern

---

## 5. TS2 Compliance Summary

| TS2 Property | Status | Evidence |
|--------------|--------|----------|
| **TS2.1: Consistent Lock Order** | ✅ PASS | Only 1 nested lock location, order is: `outbox_mutex_` → `peers_mutex_` |
| **TS2.2: No Lock Inversions** | ✅ PASS | No code path acquires `peers_mutex_` → `outbox_mutex_` |
| **TS2.3: No Blocking Under Lock** | ❌ **FAIL** | `accept_loop()` uses manual unlock to avoid blocking, but this is not TS2-compliant |
| **TS2.4: No Manual Unlock** | ❌ **FAIL** | `accept_loop()` lines 543-545 use manual unlock/lock |

---

## 6. Violations Identified

### ❌ Violation 1: Manual Unlock in accept_loop()

**Location:** `p2p_manager.cpp:543-545`

**Code:**
```cpp
std::lock_guard<std::mutex> lock(peers_mutex_);

while (!shutdown_requested_) {
    peers_mutex_.unlock();  // VIOLATION: Manual unlock
    int peer_fd = accept(listen_fd_, ...);
    peers_mutex_.lock();    // VIOLATION: Manual lock

    // ...
}
```

**TS2 Rules Violated:**
- TS2.3: Blocking operation (accept) should not require holding a lock
- TS2.4: Manual unlock/lock defeats RAII safety

**Root Cause:**
- `accept()` is a blocking system call that can wait indefinitely
- Holding `peers_mutex_` during accept would block all peer operations
- Current code "solves" this with manual unlock, but this is not TS2-compliant

**Proposed Fix:**
Refactor to separate concerns:
```cpp
// Option A: Move accept() outside lock scope entirely
int peer_fd = accept(listen_fd_, ...);  // No lock held
if (peer_fd >= 0 && !shutdown_requested_) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    // Add peer to registry
}

// Option B: Use select/poll with timeout to avoid indefinite blocking
fd_set readfds;
FD_ZERO(&readfds);
FD_SET(listen_fd_, &readfds);
struct timeval timeout = {0, 100000};  // 100ms
int ready = select(listen_fd_ + 1, &readfds, NULL, NULL, &timeout);
if (ready > 0) {
    int peer_fd = accept(listen_fd_, ...);  // Won't block
    std::lock_guard<std::mutex> lock(peers_mutex_);
    // Add peer
}
```

---

## 7. TS2 Lock Hierarchy Formal Definition

Based on analysis, the authoritative TS2 lock hierarchy for P2PManager is:

```
LockLevel::OUTBOX (1)  ≺  LockLevel::PEERS (2)
```

**Formal Statement:**
```
∀ thread T ∈ P2PManager:
  if T acquires both outbox_mutex_ and peers_mutex_,
  then acquire(outbox_mutex_) happens-before acquire(peers_mutex_)
```

**Currently Enforced Locations:**
- `broadcast_message_async()` (p2p_manager.cpp:1032-1041) ✅

**No Violations Detected For:**
- Lock order inversions ✅
- All other code paths acquire at most one lock ✅

**Violations Detected:**
- TS2.3/TS2.4: Manual unlock in `accept_loop()` ❌

---

## 8. Next Steps

### Phase 4d Refactor Tasks

1. ✅ **Step 1: Define TS2 test skeleton** - COMPLETE
   - Created `test_thread_safety_ts2.cpp` with 5 tests
   - All tests passing

2. ✅ **Step 2: Map production code** - COMPLETE
   - This document
   - Lock hierarchy identified: `outbox_mutex_` ≺ `peers_mutex_`

3. **Step 3: Identify violations** - COMPLETE
   - Found 1 violation: Manual unlock in `accept_loop()`
   - No lock order inversions found

4. ⏭️ **Step 4: Refactor to satisfy TS2** - NEXT
   - Fix `accept_loop()` manual unlock pattern
   - Options: Use select/poll with timeout OR restructure to avoid lock during accept

5. **Step 5: Add TS2 instrumentation (optional)**
   - Consider adding debug-mode lock order checking to production code
   - Use `LockOrderTracker` pattern from tests

6. **Step 6: Stress test under TSAN**
   - Build with `-fsanitize=thread`
   - Run all P2P tests under ThreadSanitizer
   - Verify no data races or lock order issues

7. **Step 7: Tag completion**
   - After all tests pass and TSAN clean
   - Tag: `v1.3.5-ring3-phase4d-ts2`

---

## 9. Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Manual unlock in accept_loop() | **MEDIUM** | Refactor to use select/poll or move accept outside lock |
| Future code might violate lock order | LOW | TS2 tests will catch violations |
| Lock hierarchy may need expansion | LOW | Current 2-mutex hierarchy is simple and sufficient |

---

## 10. TS2 Test Coverage

Current TS2 test suite (`test_thread_safety_ts2.cpp`):

| Test | Purpose | Status |
|------|---------|--------|
| TS2.1: LockOrderViolation | Detects lock order inversions | ✅ PASS |
| TS2.2: CorrectLockOrder | Verifies compliant code passes | ✅ PASS |
| TS2.3: ConcurrentOperationsNoDeadlock | Stress test for deadlocks | ✅ PASS |
| TS2.4: ShutdownNoDeadlock | Shutdown safety | ✅ PASS |
| TS2.5: LockAcquisitionStatistics | Hierarchy verification | ✅ PASS |

**All 5 tests passing** ✅

---

## Conclusion

**Production Code TS2 Status:** ⚠️ **Nearly Compliant - 1 Violation Found**

**Violations:**
1. Manual unlock in `accept_loop()` (lines 543-545) - TS2.3/TS2.4 violation

**Strengths:**
- Lock hierarchy is simple (only 2 mutexes)
- Only 1 location with nested locks
- No lock order inversions detected
- Most code uses single locks (TS2-compliant)

**Next Action:**
Refactor `accept_loop()` to eliminate manual unlock/lock pattern using select/poll approach.
