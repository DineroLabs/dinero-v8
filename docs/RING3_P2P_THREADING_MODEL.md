# Ring 3 Phase 4a: P2P Threading & Lifecycle Formal Design

**Date**: 2026-01-02
**Status**: DESIGN (No Implementation Yet)
**Purpose**: Extend Ring 3 correctness guarantees from protocol to threading

---

## Executive Summary

Ring 3 Phases 1-3 proved **protocol correctness** using simulated execution:
- ✅ Message ordering
- ✅ State machine validity
- ✅ Resource bounds (abstract)

They explicitly **do NOT model**:
- ❌ OS threads (real `std::thread` lifecycle)
- ❌ Blocking I/O (`recv()`, `send()`)
- ❌ Object lifetimes across threads
- ❌ Mutex ownership and ordering
- ❌ Destruction races

**Phase 4a Goal**: Define what "correct P2P threading" formally means, without touching production code.

---

## 1. Why Phase 4a Exists (Boundary Clarification)

### Ring 3 Phases 1-3: Protocol Model

**What they proved**:
```
∀ message sequences S:
  - Handshake completes or times out
  - Messages arrive in-order
  - Disconnection frees resources (within model)
  - State transitions are valid
```

**Model assumptions** (deliberate simplifications):
- `DeterministicScheduler` - simulated time, no real threads
- `MockSocket` - simulated I/O, no blocking
- Single-threaded event loop - no concurrency

**Result**: 66 tests passing, ~15,000 property checks validated

### The Gap: Production Code Uses Real Threading

**Production reality** (`src/daemon/p2p_manager.cpp`):
- Real `std::thread` objects (can be joined, detached, or dangling)
- Real blocking I/O (`recv()` blocks indefinitely)
- Real mutex contention (deadlocks possible)
- Real object lifetimes (use-after-free possible)

**Observed failures**:
- Test #61: P2PHandshakeVerification (SIGSEGV during cleanup)
- Test #62: P2PPingPongVerification (SIGSEGV)
- Test #92: MempoolAncestorDescendant (SIGSEGV)

**Root cause**: Threading bugs in code **outside Ring 3 model**.

### Phase 4a Solution: Extend the Model

**Not**: "Fix the bugs empirically"
**Instead**: "Define what correctness means, then prove fixes satisfy it"

Ring 3 discipline:
```
Property fails → Fix applied → Property passes
```

Current state violates this:
```
❌ No property captures the segfault (real threads not modeled)
✅ Bugs identified
❌ Fixes attempted but still crash
❌ No property validates correctness
```

**Phase 4a**: Define the missing properties **without implementing fixes**.

---

## 2. Phase 4a Deliverable (Design Only)

### This Document

**File**: `docs/RING3_P2P_THREADING_MODEL.md`

**Contents**:
1. Thread ownership model
2. Lifetime state machine (orthogonal to protocol state)
3. Lock ordering rules
4. Destruction semantics
5. Shutdown protocol
6. Future properties (not implemented yet)

**Constraints**:
- ✅ No production code changes
- ✅ No test changes
- ✅ No experimental fixes
- ✅ Pure design

### Exit Criteria

Phase 4a is complete when:
- [ ] Threading model document exists
- [ ] Lifetime state machine defined
- [ ] Ownership rules written
- [ ] Lock order specified
- [ ] Shutdown protocol formalized
- [ ] Future properties listed

**Then**: Commit documentation, tag `v1.3.3-ring3-phase4a-design`

---

## 3. Core Abstractions (Formal)

### 3.1 Thread Domains

Define exactly three thread domains:

| Domain | Responsibility | Allowed Operations |
|--------|---------------|-------------------|
| **Manager Thread** | Owns peer registry | create / destroy peers |
| **Peer Thread** | Runs peer message loop | send / receive / timeout |
| **IO Thread** (optional) | Socket I/O only | read/write (no lifecycle) |

**Invariant T1** (Thread Ownership):
```
∀ peer P:
  P is owned by exactly one domain at any time
```

**Invariant T2** (Domain Isolation):
```
∀ operations O on peer P:
  O executes in P's owning domain
  OR O is explicitly transferred to another domain
```

**Invariant T3** (No Cross-Domain Access):
```
∀ threads T₁, T₂ in different domains:
  T₁ and T₂ may not concurrently modify peer P
  (unless protected by explicit synchronization)
```

### 3.2 Peer Lifetime States (NEW)

**Critical**: This is **orthogonal** to the protocol state machine.

Protocol states (Ring 3 Phases 1-3):
```
DISCONNECTED → CONNECTING → HANDSHAKING → ESTABLISHED → DISCONNECTING
```

Lifetime states (Ring 3 Phase 4a):
```
ALLOCATED → RUNNING → STOPPING → JOINED → DESTROYED
```

**Lifetime state transitions**:

```
ALLOCATED
  ↓ (peer thread spawned)
RUNNING
  ↓ (shutdown_requested OR disconnect initiated)
STOPPING
  ↓ (peer thread exits)
JOINED
  ↓ (removed from registry)
DESTROYED
```

**Invariant L1** (Safe Destruction):
```
∀ peer P:
  P may only be destroyed in state JOINED
```

**Invariant L2** (No Access After Stopping):
```
∀ peer P, thread T:
  if lifetime_state(P) ≥ STOPPING:
    T may not access P (except to join)
```

**Invariant L3** (Monotonicity):
```
∀ peer P:
  lifetime_state(P) never decreases
  (no "un-joining" or "un-stopping")
```

**Invariant L4** (Thread Exit Before Destruction):
```
∀ peer P:
  state(P) = JOINED → peer_thread(P) has exited
```

### 3.3 Relationship: Protocol State × Lifetime State

Not all combinations are valid:

| Protocol State | Valid Lifetime States |
|---------------|----------------------|
| DISCONNECTED | ALLOCATED, DESTROYED |
| CONNECTING | ALLOCATED, RUNNING |
| HANDSHAKING | RUNNING |
| ESTABLISHED | RUNNING |
| DISCONNECTING | STOPPING, JOINED |

**Invariant PL1** (Protocol-Lifetime Consistency):
```
∀ peer P:
  protocol_state(P) = ESTABLISHED → lifetime_state(P) = RUNNING
```

**Invariant PL2** (Destruction Precondition):
```
∀ peer P:
  lifetime_state(P) = DESTROYED → protocol_state(P) = DISCONNECTED
```

---

## 4. Ownership & Memory Rules

### 4.1 Ownership Model

**Single legal ownership model**:

```cpp
// Manager owns peers via shared_ptr
std::unordered_map<std::string, std::shared_ptr<PeerInfo>> connected_peers_;

// Worker threads hold ONLY weak_ptr
std::weak_ptr<PeerInfo> peer_weak;

// Raw pointers forbidden across thread boundaries
// (OK within single stack frame for brevity)
```

**Invariant O1** (No Cross-Thread Raw Pointers):
```
∀ thread T, peer P:
  T may not hold a raw pointer to P across suspension points
  (suspension points: locks, I/O, sleeps)
```

**Rationale**: Raw pointers can dangle if another thread destroys the peer.

**Invariant O2** (Thread Exit Before Last Reference):
```
∀ peer P:
  peer_thread(P) must exit before last shared_ptr<P> is released
```

**Rationale**: Otherwise destructor runs concurrently with thread execution.

**Invariant O3** (Registry Holds Last Reference):
```
∀ peer P with lifetime_state(P) < JOINED:
  ∃ shared_ptr<P> in connected_peers_
```

**Rationale**: Ensures peer is not destroyed while thread is still running.

### 4.2 Memory Access Rules

**Invariant M1** (Lock Protection):
```
∀ peer P, thread T:
  if T modifies P:
    T holds peers_mutex_ OR T is P's owning thread
```

**Invariant M2** (Weak Pointer Upgrade):
```
∀ thread T holding weak_ptr<P>:
  before accessing P:
    auto locked = weak_ptr.lock()
    if (!locked) → abort operation (peer destroyed)
```

**Invariant M3** (No Modification After Stopping):
```
∀ peer P with lifetime_state(P) ≥ STOPPING:
  no thread may modify P (read-only access only)
```

---

## 5. Locking Rules (Critical)

### 5.1 Global Lock Order

**Define once, enforce forever**:

```
manager_mutex_ → peers_mutex_ → socket_mutex_
```

**Invariant LO1** (Lock Order Preservation):
```
∀ thread T, mutexes M₁, M₂:
  if order(M₁) < order(M₂):
    T may not acquire M₂ while holding M₁
```

**Rationale**: Prevents deadlock.

**Example violation** (from production code):
```cpp
std::lock_guard<std::mutex> lock(peers_mutex_);  // Acquire peers_mutex_
// ...
peers_mutex_.unlock();  // Release peers_mutex_
connect_to_peer();      // May acquire manager_mutex_ (wrong order!)
peers_mutex_.lock();    // Re-acquire peers_mutex_
```

### 5.2 RAII Lock Discipline

**Invariant LO2** (No Manual Unlock):
```
∀ mutex M owned by lock_guard<M>:
  M may not be manually unlocked via M.unlock()
```

**Rationale**: `lock_guard` destructor will try to unlock again → undefined behavior.

**Legal patterns**:
```cpp
// ✅ CORRECT: lock_guard (no manual unlock)
{
    std::lock_guard<std::mutex> lock(peers_mutex_);
    // ...
} // Automatic unlock

// ✅ CORRECT: unique_lock (manual unlock allowed)
std::unique_lock<std::mutex> lock(peers_mutex_);
// ...
lock.unlock();  // Explicit unlock
// ...
lock.lock();    // Explicit re-lock
```

**Illegal patterns**:
```cpp
// ❌ WRONG: lock_guard + manual unlock
std::lock_guard<std::mutex> lock(peers_mutex_);
peers_mutex_.unlock();  // UNDEFINED BEHAVIOR
```

### 5.3 No Blocking Under Lock

**Invariant LO3** (No Blocking Operations While Holding Lock):
```
∀ thread T, mutex M:
  while T holds M:
    T may not execute:
      - join()
      - sleep_for() / sleep_until()
      - recv() / send() (blocking I/O)
      - condition_variable.wait() (unless M is the CV's mutex)
```

**Rationale**: Blocking while holding a lock causes other threads to stall.

**Example violation** (hypothetical):
```cpp
std::lock_guard<std::mutex> lock(peers_mutex_);
peer_thread.join();  // ❌ Deadlock if peer thread needs peers_mutex_
```

**Correct pattern**:
```cpp
// Release lock before blocking operation
{
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peer->is_stopping = true;
} // Lock released

peer_thread.join();  // ✅ Safe (no lock held)
```

---

## 6. Shutdown Semantics (The Missing Spec)

### Current Production Bug

**Observed behavior**:
```cpp
void P2PManager::stop() {
    shutdown_requested_ = true;

    // Join peer threads
    for (auto& thread : peer_threads_) {
        thread->join();  // Thread may still access peers
    }

    // Cleanup peers
    for (auto& pair : connected_peers_) {
        close_socket(pair.second->socket_fd);  // Use-after-free possible
    }
    connected_peers_.clear();  // Destroys peers
}
```

**Problem**: Thread may be executing `peer_ptr->is_connected` while peer is being destroyed.

### Correct Shutdown Protocol (Three-Phase)

**Phase S1: Signal**
```
Set peer->is_running = false
Wake up blocked threads (close sockets or notify CV)
No deletion allowed
```

**Phase S2: Join**
```
Join peer thread
No locks held during join
Thread must exit cleanly (no more access to peer)
```

**Phase S3: Destroy**
```
Remove from registry
Release last owner (shared_ptr)
Destructor runs single-threaded
```

**Invariant S1** (No Concurrent Execution):
```
∀ peer P:
  Destruction is never concurrent with execution
```

**Invariant S2** (No Timing Assumptions):
```
∀ shutdown sequences:
  No sleeps, delays, or timing assumptions are permitted
  (Thread exit must be signaled explicitly, not inferred from time)
```

**Invariant S3** (Signal Before Join):
```
∀ peer P:
  is_running(P) = false BEFORE join(peer_thread(P))
```

**Invariant S4** (Join Before Destroy):
```
∀ peer P:
  peer_thread(P) joined BEFORE connected_peers_.erase(P)
```

### Correct Implementation Pattern (Design, Not Code)

```
Algorithm: shutdown_peer(P)
  Input: peer P with lifetime_state(P) = RUNNING
  Output: peer P with lifetime_state(P) = DESTROYED

  1. Signal Phase (RUNNING → STOPPING)
     a. Acquire peers_mutex_
     b. Set P.is_running = false
     c. Set P.lifetime_state = STOPPING
     d. Close P.socket_fd (unblock recv/send)
     e. Release peers_mutex_

  2. Join Phase (STOPPING → JOINED)
     a. Join peer_thread(P)  // No lock held
     b. Acquire peers_mutex_
     c. Set P.lifetime_state = JOINED
     d. Release peers_mutex_

  3. Destroy Phase (JOINED → DESTROYED)
     a. Acquire peers_mutex_
     b. connected_peers_.erase(P)  // Last reference dropped
     c. P destructor runs (automatically)
     d. Release peers_mutex_
     e. P.lifetime_state = DESTROYED (conceptual, object is gone)

  Postcondition:
    ∀ invariants I: I holds
```

**Note**: This is **design**, not a patch. Implementation comes in Phase 4b.

---

## 7. New Ring 3.5 Properties (Design Only)

These are **future tests**, not implemented yet. Phase 4b will implement them.

### 7.1 Thread Safety Properties

**Property TS1** (No Use-After-Free):
```
∀ executions E, peer P, thread T:
  if T accesses P.field:
    P.lifetime_state < DESTROYED
```

**Test strategy** (future):
- Run with Address Sanitizer (ASAN)
- Fuzz shutdown sequences
- Verify no ASAN violations

**Property TS2** (Single Destruction):
```
∀ peer P:
  destructor(P) executes exactly once
```

**Test strategy** (future):
- Instrument destructors with atomic counter
- Run stress test (1000s of connect/disconnect cycles)
- Verify counter = number of peers created

**Property TS3** (Thread Exit Before Destruction):
```
∀ peer P:
  if lifetime_state(P) = DESTROYED:
    peer_thread(P) exited before destructor(P) ran
```

**Test strategy** (future):
- Log thread exit timestamps
- Log destructor entry timestamps
- Verify exit_time < destructor_time (for all peers)

### 7.2 Lock Ordering Properties

**Property LO1** (No Deadlock):
```
∀ executions E:
  no thread T is blocked waiting for mutex M owned by thread T'
  while T' is blocked waiting for mutex M' owned by T
```

**Test strategy** (future):
- Run with Thread Sanitizer (TSAN)
- Stress test with many concurrent connects/disconnects
- Verify no TSAN deadlock warnings

**Property LO2** (Lock Order Respected):
```
∀ thread T, mutexes M₁, M₂:
  if T holds M₁ and acquires M₂:
    order(M₁) < order(M₂)
```

**Test strategy** (future):
- Instrument mutex locks with stack trace logging
- Verify all lock acquisitions respect global order

### 7.3 Blocking Under Lock Properties

**Property BL1** (No Blocking Operations Under Lock):
```
∀ thread T, mutex M:
  while T holds M:
    T does not call {join, sleep, recv, send}
```

**Test strategy** (future):
- Instrument blocking calls
- Log mutex ownership state
- Verify no blocking call occurs while lock held

### 7.4 Shutdown Properties

**Property SD1** (Clean Shutdown):
```
∀ P2PManager instances M:
  M.stop() completes without deadlock, segfault, or timeout
```

**Test strategy** (future):
- Run stop() 1000 times in different states
- Verify all complete within bounded time (e.g., 5 seconds)

**Property SD2** (No Resource Leaks):
```
∀ P2PManager instances M:
  after M.stop():
    all sockets closed
    all threads joined
    all memory freed
```

**Test strategy** (future):
- Run with Valgrind
- Verify no memory leaks
- Verify no file descriptor leaks

---

## 8. Why This Is Phase 4a (Not 4b)

### What Phase 4a Is

**Phase 4a = Design**:
- ✅ Define invariants
- ✅ Define properties
- ✅ Specify correct behavior
- ✅ Document model extensions

**No implementation**:
- ❌ No production code changes
- ❌ No test additions
- ❌ No experimental fixes
- ❌ No "let's try this" patches

### Why Not Fix Bugs Now?

**Current state**:
```
❌ No formal definition of correctness
   (We know it crashes, but not what "correct" means formally)

❌ Any fix would be empirical
   ("It doesn't crash" ≠ "It's correct")

❌ Ring discipline violated
   (property → fix → verify, NOT fix → hope)
```

**After Phase 4a**:
```
✅ Correctness formally defined
   (Invariants O1-O3, L1-L4, S1-S4, etc.)

✅ Properties specified
   (Future tests TS1-TS3, LO1-LO2, BL1, SD1-SD2)

✅ Fixes can be proven
   ("Property TS1 failed → fix applied → TS1 now passes")
```

### Ring 3 Methodology Preserved

**Ring 3 contract**:
```
1. Property fails (captures bug)
2. Fix applied
3. Property passes (proves correctness)
```

**Phase 4a ensures**:
- Step 1 is possible (properties exist)
- Step 3 is meaningful (properties are correct)

**Without Phase 4a**:
- Step 1: ??? (no property)
- Step 2: Guess-and-check
- Step 3: ??? (no validation)

---

## 9. Exit Criteria for Phase 4a

Phase 4a is **complete** when:

- [x] Threading model document exists (`docs/RING3_P2P_THREADING_MODEL.md`)
- [x] Thread domains defined (Section 3.1)
- [x] Lifetime state machine defined (Section 3.2)
- [x] Ownership rules written (Section 4)
- [x] Lock order specified (Section 5)
- [x] Shutdown protocol formalized (Section 6)
- [x] Future properties listed (Section 7)

**Then**:
1. Commit documentation only
2. Tag as `v1.3.3-ring3-phase4a-design`
3. Push to origin

**No code changes. No tests added.**

---

## 10. What Comes After (Not Now)

### Phase 4b: Refactor to Model

**Goal**: Make production code satisfy Phase 4a invariants

**Tasks**:
- Replace raw pointers with `shared_ptr` / `weak_ptr`
- Fix lock ordering (change `lock_guard` to `unique_lock` where needed)
- Implement three-phase shutdown
- Add lifetime state tracking

**Exit criteria**: All invariants O1-O3, L1-L4, S1-S4 satisfied

### Phase 4c: Add Real-Thread Properties

**Goal**: Implement properties TS1-TS3, LO1-LO2, BL1, SD1-SD2

**Tasks**:
- Add ASAN/TSAN to build
- Write property tests using real threads
- Add stress tests (1000s of cycles)

**Exit criteria**: All properties pass

### Phase 4d: Fix Production Tests

**Goal**: Make Tests #61, #62, #92 pass

**Tasks**:
- Apply Phase 4b refactorings
- Run tests with ASAN/TSAN
- Verify no segfaults

**Exit criteria**: All production P2P tests pass

### Phase 4e: Remove from Known Issues

**Goal**: Close Ring 3 Phase 4

**Tasks**:
- Update `RING3_P2P_KNOWN_ISSUES.md`
- Mark Issues #1-#3 as FIXED
- Document which properties prove correctness

**Exit criteria**: Ring 3 complete, all tests passing

---

## Summary: Phase 4a Deliverable

### What This Document Defines

1. **Thread Domains** (Section 3.1)
   - Manager, Peer, IO threads
   - Invariants T1-T3

2. **Lifetime States** (Section 3.2)
   - ALLOCATED → RUNNING → STOPPING → JOINED → DESTROYED
   - Invariants L1-L4, PL1-PL2

3. **Ownership Model** (Section 4)
   - `shared_ptr` for ownership
   - `weak_ptr` for worker threads
   - No raw pointers across threads
   - Invariants O1-O3, M1-M3

4. **Locking Rules** (Section 5)
   - Global lock order: manager → peers → socket
   - RAII discipline (no manual unlock with `lock_guard`)
   - No blocking under lock
   - Invariants LO1-LO3

5. **Shutdown Protocol** (Section 6)
   - Three phases: Signal → Join → Destroy
   - No timing assumptions
   - Invariants S1-S4

6. **Future Properties** (Section 7)
   - Thread safety (TS1-TS3)
   - Lock ordering (LO1-LO2)
   - Blocking detection (BL1)
   - Shutdown correctness (SD1-SD2)

### What This Document Does NOT Do

- ❌ Change production code
- ❌ Add tests
- ❌ Fix bugs
- ❌ Make claims about current code
- ❌ Propose patches

### How This Preserves Frozen Core

**Ring 3 discipline**:
```
Design → Properties → Implementation → Verification
```

**Current status**:
```
Phases 1-3: ✅ Protocol design → properties → implementation → verified
Phase 4a:   ✅ Threading design (this document)
Phase 4b:   ⏸️  Properties (future)
Phase 4c:   ⏸️  Implementation (future)
Phase 4d:   ⏸️  Verification (future)
```

**Frozen core integrity**: ✅ Preserved (documentation only, no code changes)

---

## Document Metadata

- **Created**: 2026-01-02
- **Author**: Claude Sonnet 4.5 (via Claude Code)
- **Purpose**: Formal specification of P2P threading model (Ring 3 Phase 4a)
- **Status**: Design complete, awaiting implementation (Phase 4b)

**Next Action**: Commit this document, tag `v1.3.3-ring3-phase4a-design`

**DO NOT**: Touch production code until Phase 4b begins.
