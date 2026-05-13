# Ring 3 Phase 4e: TS3 — Blocking-Free Event Loops (Specification)

**Date:** 2026-01-02
**Status:** 📋 SPECIFICATION (no code yet)
**Phase:** 4e (follows TS1 safety, TS2 deadlock freedom)

---

## Executive Summary

TS3 is the **liveness** property for P2P threading. While TS1 proves safety (no use-after-free) and TS2 proves deadlock freedom, TS3 proves **progress** — that work eventually completes and threads don't starve.

**Category:** Liveness (not safety)
**Scope:** Event loops, condition variables, shutdown responsiveness
**Goal:** Prove bounded-time progress under all conditions

---

## Formal Property: TS3

### TS3: Blocking-Free Event Loops

**Formal Statement:**
```
∀ event loop E, ∀ time T:
  If work is available and no stop signal is active,
  then E makes progress within bounded time Δt

Where:
  - "progress" means: work item dequeued and processed
  - "bounded time" means: Δt < ∞ (no indefinite blocking)
  - "stop signal" means: shutdown_requested_ == true
```

**Informal Statement:**
> If there's work to do and we're not shutting down,
> the event loop will pick it up and process it within a finite time.

**What TS3 Forbids:**
```
❌ Indefinite blocking on wait()
❌ Starvation (work available but never processed)
❌ Livelock (spinning but not progressing)
❌ Shutdown hangs (stop requested but threads don't exit)
❌ Missed wakeups (notify sent but thread still sleeping)
❌ Unbounded wait_for timeouts (no defensive wakeup)
```

**What TS3 Requires:**
```
✅ All waits have timeouts or are interruptible
✅ Shutdown signals wake all sleeping threads
✅ Work arrival guarantees eventual processing
✅ No thread can permanently starve
✅ Event loops are fair (FIFO or bounded priority)
```

---

## Formal Components

### 1. Event Loop Definition

An **event loop** E is a thread that:
1. Waits for work (via condition variable or polling)
2. Processes work items
3. Returns to step 1

**Examples in P2PManager:**
- `accept_loop()` - waits for incoming connections
- `connection_manager_loop()` - periodic peer maintenance
- `keepalive_loop()` - periodic PING sending
- `async_outbox_handler()` - waits for outbound messages

### 2. Work Availability

Work is **available** for event loop E if:
```
∃ work item w ∈ work_queue(E):
  w is ready to process
```

**Examples:**
- Accept loop: `socket has pending connection`
- Outbox handler: `outbox_queue_.size() > 0`
- Keepalive: `now - last_ping_sent >= 30s`

### 3. Progress Definition

Event loop E **makes progress** if:
```
∃ work item w:
  w is dequeued from work_queue(E) AND
  w is processed (handler invoked)
```

**Non-Progress Examples:**
- Thread sleeping while work is queued
- Thread spinning without dequeuing
- Thread blocked on I/O forever

### 4. Bounded Time Guarantee

**Bounded time** Δt means:
```
∃ constant K < ∞:
  work_available(E, t₀) ⇒ progress(E, t₁)
  where t₁ - t₀ ≤ K
```

**Practical Interpretation:**
- If work arrives at time T, it's processed by time T + Δt
- Δt depends on system load, but is **always finite**
- No worst-case scenario leads to indefinite delay

---

## TS3 Sub-Properties

### TS3.1: Wait Interruptibility

**Property:**
```
∀ thread T waiting on condition variable cv:
  shutdown_requested_ = true ⇒ T wakes within Δt_wakeup
```

**What This Prevents:**
- Shutdown hangs due to threads sleeping on `wait()`
- Missed `notify_all()` signals

**Requirements:**
- All `wait()` calls use predicates
- All `wait()` have timeout variants OR shutdown checks
- `stop()` calls `notify_all()` on all condition variables

### TS3.2: Work Queue Fairness

**Property:**
```
∀ work items w₁, w₂:
  arrival(w₁) < arrival(w₂) ⇒
    ∃ bounded k: process_order(w₁) ≤ process_order(w₂) + k
```

**What This Prevents:**
- Work starvation (old work never processed)
- Priority inversion without bound

**Requirements:**
- FIFO queues preferred
- Priority queues must have starvation prevention
- No indefinite deferral

### TS3.3: Bounded Wait Timeouts

**Property:**
```
∀ wait_for(cv, timeout):
  timeout < MAX_WAIT_TIMEOUT (e.g., 1 second)
```

**What This Prevents:**
- Threads sleeping for minutes/hours
- Unresponsive shutdown due to long waits

**Requirements:**
- All `wait_for()` have bounded timeouts (≤ 1s recommended)
- Defensive wakeup even without work
- Periodic liveness checks

### TS3.4: Shutdown Responsiveness

**Property:**
```
∀ thread T in P2PManager:
  stop() invoked at t₀ ⇒ T exits by t₀ + Δt_shutdown
  where Δt_shutdown < 5 seconds
```

**What This Prevents:**
- Graceful shutdown taking minutes
- User frustration (daemon won't stop)

**Requirements:**
- All loops check `shutdown_requested_` frequently
- All blocking operations have timeouts
- `stop()` is idempotent and fast

### TS3.5: No Livelock

**Property:**
```
∀ event loop E:
  CPU_busy(E) ∧ ¬making_progress(E) ⇒ violation
```

**What This Prevents:**
- Threads spinning without doing useful work
- Busy-waiting on conditions

**Requirements:**
- No spin loops without backoff
- No tight polling loops
- Use condition variables, not polling

---

## Current P2PManager Event Loops (Inventory)

### Loop 1: accept_loop()

**Location:** `p2p_manager.cpp:~500`

**Current Behavior:**
```cpp
while (!shutdown_requested_) {
    int client_socket = accept(listen_socket, ...);  // BLOCKS
    if (client_socket >= 0) {
        handle_incoming_connection(client_socket, ...);
    }
}
```

**TS3 Status:** ⚠️ **UNKNOWN**
- **Question:** How long can `accept()` block?
- **Question:** Does shutdown wake `accept()`?
- **Question:** Is there a timeout?

**TS3 Risk:** HIGH (blocking syscall, no visible timeout)

### Loop 2: connection_manager_loop()

**Location:** `p2p_manager.cpp:531`

**Current Behavior:**
```cpp
while (!shutdown_requested_) {
    // Try to connect to seed nodes
    // ...

    // Send keepalive pings
    // ...

    std::this_thread::sleep_for(std::chrono::seconds(1));  // BLOCKS
}
```

**TS3 Status:** ✅ **LIKELY COMPLIANT**
- Sleep is bounded (1 second)
- Loop checks `shutdown_requested_` each iteration
- Progress: seed connections + keepalives

**TS3 Risk:** LOW (bounded sleep, frequent shutdown checks)

### Loop 3: async_outbox_handler()

**Location:** `p2p_manager.cpp:~1271`

**Current Behavior:**
```cpp
while (!shutdown_requested_) {
    {
        std::unique_lock<std::mutex> lock(outbox_mutex_);
        outbox_cv_.wait_for(lock, std::chrono::milliseconds(100),
            [this]{ return shutdown_requested_ || !outbox_queue_.empty(); });

        if (shutdown_requested_) break;

        if (outbox_queue_.empty()) continue;

        msg = std::move(outbox_queue_.front());
        outbox_queue_.pop_front();
    }

    // Process message (non-blocking send)
}
```

**TS3 Status:** ✅ **COMPLIANT**
- `wait_for()` has 100ms timeout
- Predicate checks `shutdown_requested_`
- Work processed when available

**TS3 Risk:** VERY LOW (textbook condition variable usage)

### Loop 4: keepalive_loop()

**Location:** `p2p_manager.cpp:~1400`

**Current Behavior:**
```cpp
while (!shutdown_requested_) {
    // Send PINGs to peers
    // ...

    std::this_thread::sleep_for(std::chrono::seconds(30));  // BLOCKS
}
```

**TS3 Status:** ⚠️ **MARGINAL**
- Sleep is bounded (30 seconds) ✅
- But shutdown could take up to 30s ❌
- No defensive wakeup on shutdown signal

**TS3 Risk:** MEDIUM (slow shutdown response)

---

## TS3 Violations (Hypothetical)

These are scenarios TS3 is designed to prevent:

### Violation 1: Indefinite Accept Block

**Scenario:**
```cpp
// accept() blocks forever if no connections arrive
while (!shutdown_requested_) {
    int socket = accept(listen_fd, ...);  // No timeout
    handle(socket);
}
```

**Problem:** `stop()` called, but thread still in `accept()` waiting forever.

**TS3 Fix:** Use `select()` or `poll()` with timeout before `accept()`.

### Violation 2: Missed Wakeup

**Scenario:**
```cpp
// Thread A
cv.wait(lock, [&]{ return has_work; });

// Thread B (producer)
has_work = true;
// Forgot to call cv.notify_one()!
```

**Problem:** Work available, but consumer never wakes.

**TS3 Fix:** All state changes that unblock predicates must notify.

### Violation 3: Work Starvation

**Scenario:**
```cpp
// Priority queue always picks high-priority work
while (!shutdown_requested_) {
    auto work = priority_queue.top();  // Always high priority
    process(work);
}
```

**Problem:** Low-priority work never processed (starvation).

**TS3 Fix:** Bounded priority aging or FIFO fallback.

### Violation 4: Shutdown Hang

**Scenario:**
```cpp
void stop() {
    shutdown_requested_ = true;
    // Forgot to notify sleeping threads!
    for (auto& t : threads_) {
        t.join();  // Hangs forever
    }
}
```

**Problem:** Threads sleeping on `wait()`, never wake.

**TS3 Fix:** `notify_all()` on all condition variables in `stop()`.

### Violation 5: Livelock

**Scenario:**
```cpp
while (!shutdown_requested_) {
    if (has_work()) {
        // Oops, forgot to actually dequeue and process!
        continue;  // Spin forever
    }
}
```

**Problem:** CPU pegged at 100%, no progress.

**TS3 Fix:** Ensure work is actually dequeued and processed.

---

## TS3 Testing Strategy

### 1. Unit Tests (Property-Based)

**Test:** Bounded wakeup time
```cpp
TEST(TS3, BoundedWakeupOnShutdown) {
    Manager m;
    m.start();

    auto start = now();
    m.stop();
    auto duration = now() - start;

    EXPECT_LT(duration, 5s);  // TS3.4 compliance
}
```

**Test:** Work eventually processed
```cpp
TEST(TS3, WorkEventuallyProcessed) {
    Manager m;
    m.start();

    m.queue_work(work_item);

    // Wait bounded time
    EXPECT_TRUE(wait_for([&]{ return work_processed; }, 1s));
}
```

**Test:** No starvation
```cpp
TEST(TS3, NoStarvation) {
    Manager m;
    m.start();

    // Queue 1000 work items
    for (int i = 0; i < 1000; i++) {
        m.queue_work(item[i]);
    }

    // All items processed within bounded time
    EXPECT_TRUE(all_processed_within(10s));
}
```

### 2. Stress Tests

**Test:** Shutdown under load
```cpp
TEST(TS3, ShutdownUnderLoad) {
    Manager m;
    m.start();

    // Flood with work
    std::thread producer([&]{
        while (true) m.queue_work(...);
    });

    std::this_thread::sleep_for(1s);

    // Stop should complete quickly despite load
    auto start = now();
    m.stop();
    auto duration = now() - start;

    EXPECT_LT(duration, 5s);
}
```

### 3. Adversarial Tests

**Test:** Missed wakeup detection
```cpp
TEST(TS3, MissedWakeupDetection) {
    // Inject fault: skip notify_one()
    // Verify work still processed (timeout wakeup)
}
```

**Test:** Livelock detection
```cpp
TEST(TS3, LivelockDetection) {
    // Monitor: CPU usage vs progress ratio
    // If CPU high but progress low → livelock
}
```

---

## TS3 vs TS1/TS2 (Comparison)

| Property | Category | What It Proves | Example Violation |
|----------|----------|----------------|-------------------|
| **TS1** | Safety | No use-after-free | Thread accesses deleted peer |
| **TS2** | Deadlock Freedom | No circular wait | Thread A waits for B, B waits for A |
| **TS3** | Liveness | Progress guaranteed | Work queued but never processed |

**Key Difference:**
- TS1/TS2: "Bad things don't happen"
- TS3: "Good things eventually happen"

**Independence:**
- TS1 + TS2 + TS3 are **orthogonal**
- You can satisfy TS1/TS2 but violate TS3 (safe but starved)
- You can satisfy TS3 but violate TS1/TS2 (progresses but crashes)

---

## Success Criteria (TS3 Proven)

TS3 is **proven** when:

1. ✅ All event loops have bounded wait times
2. ✅ All condition variables have timeouts or shutdown signals
3. ✅ Work queues are fair (FIFO or bounded priority)
4. ✅ Shutdown completes within 5 seconds under any load
5. ✅ No starvation scenarios exist
6. ✅ Property tests pass (bounded time, no livelock)

---

## Next Steps

### Phase 4e Roadmap

1. **Define TS3 formally** ✅ (this document)

2. **Write TS3 tests (expect failures)**
   - Create `test_thread_safety_ts3.cpp`
   - 5-10 property tests
   - Tests will likely FAIL initially

3. **Audit production code for TS3 violations**
   - Map all event loops
   - Check all `wait()` calls
   - Verify timeouts exist

4. **Fix violations (if any)**
   - Add timeouts to blocking calls
   - Add `notify_all()` to `stop()`
   - Convert long sleeps to interruptible waits

5. **Validate TS3 compliance**
   - All tests pass
   - Stress tests show bounded shutdown time
   - No starvation under load

6. **Document and seal**
   - TS3 compliance report
   - Tag: `v1.3.8-ring3-phase4e-ts3`
   - Ring 3 threading COMPLETE

---

## Open Questions (To Be Answered During Implementation)

1. **Does `accept()` have a timeout?**
   - Or is it using `select()`/`poll()` first?
   - Shutdown responsiveness depends on this

2. **Are all condition variables notified on shutdown?**
   - Need to verify `stop()` implementation
   - Missing `notify_all()` breaks TS3.4

3. **What is the worst-case shutdown time?**
   - Currently unknown
   - TS3 requires it to be bounded and measured

4. **Are there any polling loops?**
   - Polling violates TS3.5 (livelock risk)
   - Should use condition variables instead

5. **What happens under heavy load?**
   - Does work queue have a size limit?
   - Does backpressure cause starvation?

---

## Conclusion

**TS3 is the final piece** of Ring 3 threading formal verification.

**Status:**
- TS1 (Safety): ✅ Proven
- TS2 (Deadlock Freedom): ✅ Proven
- TS3 (Liveness): 📋 Specification complete, implementation pending

**Next Action:** Create `test_thread_safety_ts3.cpp` with property tests.

**Expected Outcome:** Some tests will fail initially, revealing liveness issues that TS1/TS2 don't cover.

**Final State:** Ring 3 threading will be **safe, deadlock-free, AND live** — the complete threading trifecta.

---

_Ring 3 Phase 4e — TS3 Specification Complete_
_Next: Write failing tests, then fix violations_
