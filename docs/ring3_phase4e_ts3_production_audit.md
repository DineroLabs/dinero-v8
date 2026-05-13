# Ring 3 Phase 4e: TS3 Production Code Audit

**Date:** 2026-01-02
**Status:** 🔍 AUDIT COMPLETE (violations found)
**Phase:** 4e (TS3 Liveness)

---

## Executive Summary

Production P2PManager has been audited for TS3 (Blocking-Free Event Loops) compliance. **2 violations found** requiring fixes before TS3 can be proven.

**Audit Results:**
- ✅ **2 loops compliant** (listen_loop, outbox_loop)
- ⚠️ **1 loop marginal** (connection_manager_loop)
- ❌ **2 violations** (keepalive_loop, peer_handler_loop)

**Risk Level:** MEDIUM
- Shutdown can take up to 30 seconds under worst-case conditions
- Peer handler threads may block indefinitely waiting for messages

---

## TS3 Property (Reminder)

```
∀ event loop E, ∀ time T:
  If work is available and no stop signal is active,
  then E makes progress within bounded time Δt

TS3.4: Shutdown Responsiveness
  ∀ thread T: stop() invoked at t₀ ⇒ T exits by t₀ + 5s
```

---

## Event Loop Inventory

Production P2PManager has **5 event loops** (1 more than specification identified):

| Loop | Location | Function | TS3 Status |
|------|----------|----------|------------|
| 1. listen_loop() | Line 494 | Accept incoming connections | ✅ COMPLIANT |
| 2. connection_manager_loop() | Line 532 | Connect to seeds + keepalive | ⚠️ MARGINAL |
| 3. outbox_loop() | Line 1269 | Async message sending | ✅ COMPLIANT |
| 4. keepalive_loop() | Line 1415 | Send periodic PINGs | ❌ VIOLATION |
| 5. peer_handler_loop() | Line 643 | Receive messages from peer | ❌ VIOLATION |

---

## Loop 1: listen_loop() — ✅ COMPLIANT

### Location
`src/daemon/p2p_manager.cpp:494-530`

### Code Analysis
```cpp
void P2PManager::listen_loop() {
    int listen_socket = create_listen_socket();
    if (listen_socket < 0) return;

    while (!shutdown_requested_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;          // ✅ 1 second timeout
        timeout.tv_usec = 0;

        int activity = select(listen_socket + 1, &read_fds, nullptr, nullptr, &timeout);

        if (activity > 0 && FD_ISSET(listen_socket, &read_fds)) {
            int client_socket = accept(listen_socket, ...);
            if (client_socket >= 0) {
                handle_incoming_connection(client_socket, client_address);
            }
        }
    }

    close_socket(listen_socket);
}
```

### TS3 Compliance Matrix

| Property | Status | Evidence |
|----------|--------|----------|
| **TS3.1: Wait Interruptibility** | ✅ PASS | Loop checks `shutdown_requested_` each iteration |
| **TS3.3: Bounded Wait Timeout** | ✅ PASS | `select()` timeout = 1 second |
| **TS3.4: Shutdown Responsiveness** | ✅ PASS | Wakes within 1 second |
| **TS3.5: No Livelock** | ✅ PASS | Blocks on select(), not spinning |

### Risk Assessment
**Risk:** VERY LOW ✅

**Reasoning:**
- Uses `select()` with 1-second timeout instead of bare blocking `accept()`
- Shutdown signal checked every iteration
- No blocking operations under lock
- Pattern matches mock test expectations

**Contrast with Specification:**
- Spec predicted HIGH RISK due to "blocking accept(), no visible timeout"
- Production code actually uses `select()` with timeout (safer pattern)
- Risk downgraded to VERY LOW

---

## Loop 2: connection_manager_loop() — ⚠️ MARGINAL

### Location
`src/daemon/p2p_manager.cpp:532-588`

### Code Analysis
```cpp
void P2PManager::connection_manager_loop() {
    while (!shutdown_requested_) {
        // Collect seed connection target inside lock
        std::optional<std::pair<std::string, int>> seed_to_connect;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            if (connected_peers_.size() < 3 && !seed_nodes_.empty()) {
                // ... find seed to connect ...
                seed_to_connect = seed;
            }
        }

        // Connect outside lock (TS2 compliant)
        if (seed_to_connect.has_value()) {
            connect_to_peer(seed_to_connect->first, seed_to_connect->second);
        }

        // Send keepalive PINGs (every 30s)
        {
            std::vector<std::string> peers_to_ping;
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                for (auto& [peer_key, peer] : connected_peers_) {
                    if (elapsed.count() >= 30) {
                        peers_to_ping.push_back(peer_key);
                    }
                }
            }

            for (const auto& peer_key : peers_to_ping) {
                send_to_peer(peer_key, ping_msg);
            }
        }

        // ⚠️ 10 second sleep
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}
```

### TS3 Compliance Matrix

| Property | Status | Evidence |
|----------|--------|----------|
| **TS3.1: Wait Interruptibility** | ✅ PASS | Loop checks `shutdown_requested_` each iteration |
| **TS3.3: Bounded Wait Timeout** | ✅ PASS | Sleep = 10 seconds (bounded) |
| **TS3.4: Shutdown Responsiveness** | ⚠️ MARGINAL | Can delay shutdown by up to 10 seconds |
| **TS3.5: No Livelock** | ✅ PASS | Sleeps, not spinning |

### Risk Assessment
**Risk:** LOW ⚠️

**Reasoning:**
- Sleep is bounded (10 seconds)
- **But:** 10s delay on shutdown is noticeable
- TS3.4 requires shutdown < 5 seconds total
- If this is one of the last threads to join, it could push total shutdown time to 10s

**Impact:**
- Shutdown delay: up to 10 seconds
- User experience: noticeable but not critical

**Recommendation:**
- Consider reducing sleep to 1-2 seconds OR
- Use condition variable with timeout (allows immediate wakeup on shutdown)

**Comparison to Specification:**
- Spec predicted 1-second sleep (LOW risk)
- Production code has 10-second sleep (MARGINAL risk)
- Still within TS3.3 bounds, but violates TS3.4 target

---

## Loop 3: outbox_loop() — ✅ COMPLIANT

### Location
`src/daemon/p2p_manager.cpp:1269-1356`

### Code Analysis
```cpp
void P2PManager::outbox_loop() {
    while (!shutdown_requested_) {
        OutMsg msg;

        {
            std::unique_lock<std::mutex> lock(outbox_mutex_);

            // ✅ TEXTBOOK PATTERN: wait_for with predicate
            outbox_cv_.wait_for(lock, std::chrono::milliseconds(100),
                [this]{ return shutdown_requested_ || !outbox_queue_.empty(); });

            if (shutdown_requested_) break;  // ✅ Immediate exit on shutdown

            if (outbox_queue_.empty()) continue;

            msg = std::move(outbox_queue_.front());
            outbox_queue_.pop_front();
        }

        // Process message outside lock
        // (non-blocking send with retry logic)
    }
}
```

### TS3 Compliance Matrix

| Property | Status | Evidence |
|----------|--------|----------|
| **TS3.1: Wait Interruptibility** | ✅ PASS | Predicate checks `shutdown_requested_` |
| **TS3.3: Bounded Wait Timeout** | ✅ PASS | `wait_for()` timeout = 100ms |
| **TS3.4: Shutdown Responsiveness** | ✅ PASS | Wakes within 100ms |
| **TS3.5: No Livelock** | ✅ PASS | Blocks on CV, dequeues work when available |

### Risk Assessment
**Risk:** VERY LOW ✅

**Reasoning:**
- **Perfect TS3 pattern** (matches mock test expectations exactly)
- Condition variable with bounded timeout
- Predicate guards against spurious wakeups
- Shutdown signal integrated into wait condition
- Work processed outside lock (no blocking under lock)

**Code Quality:**
- This is a **reference implementation** for TS3 compliance
- Could be cited as example in TS3 documentation

---

## Loop 4: keepalive_loop() — ❌ VIOLATION

### Location
`src/daemon/p2p_manager.cpp:1415-1451`

### Code Analysis
```cpp
void P2PManager::keepalive_loop() {
    while (!shutdown_requested_) {
        // ❌ 30 second uninterruptible sleep
        std::this_thread::sleep_for(std::chrono::seconds(30));

        // ✅ Shutdown check (but too late)
        if (shutdown_requested_) break;

        // Send PINGs to all connected peers
        std::vector<std::string> peer_addresses;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (const auto& pair : connected_peers_) {
                if (pair.second->is_connected) {
                    peer_addresses.push_back(pair.first);
                }
            }
        }

        for (const auto& peer_addr : peer_addresses) {
            uint64_t nonce = ...;
            auto ping_msg = P2PMessage::create_ping(nonce);
            send_to_peer(peer_addr, ping_msg);
        }
    }
}
```

### TS3 Compliance Matrix

| Property | Status | Evidence |
|----------|--------|----------|
| **TS3.1: Wait Interruptibility** | ❌ **FAIL** | `sleep_for()` not interruptible |
| **TS3.3: Bounded Wait Timeout** | ✅ PASS | Sleep = 30 seconds (bounded) |
| **TS3.4: Shutdown Responsiveness** | ❌ **FAIL** | Can delay shutdown by up to 30 seconds |
| **TS3.5: No Livelock** | ✅ PASS | Sleeps, not spinning |

### Violation Details

**TS3.1 Violation:**
```
Property: shutdown_requested_ = true ⇒ T wakes within Δt_wakeup
Status: VIOLATED

Reason: std::this_thread::sleep_for() is NOT interruptible
  - If shutdown requested 1s into 30s sleep, thread sleeps for 29 more seconds
  - No mechanism to wake sleeping thread
```

**TS3.4 Violation:**
```
Property: stop() invoked at t₀ ⇒ T exits by t₀ + 5s
Status: VIOLATED

Worst Case: 30 second delay
  - User calls stop()
  - keepalive_loop thread is 1 second into 30-second sleep
  - Thread sleeps for 29 more seconds before checking shutdown_requested_
  - Total shutdown time: 29+ seconds
```

### Impact Assessment

**User Impact:**
- `dinero-cli stop` command takes up to 30 seconds to complete
- User perceives daemon as "hung" or unresponsive
- Poor user experience

**Production Impact:**
- SIGTERM during system shutdown may timeout
- Init systems (systemd) may SIGKILL after grace period
- Unclean shutdown risk

**Severity:** **MEDIUM-HIGH**

### Risk Assessment
**Risk:** HIGH ❌

**Reasoning:**
- Direct violation of TS3.4 (shutdown < 5s)
- User-visible impact (slow shutdown)
- No technical reason for 30s uninterruptible sleep

---

## Loop 5: peer_handler_loop() — ❌ VIOLATION

### Location
`src/daemon/p2p_manager.cpp:643-730`

### Code Analysis
```cpp
void P2PManager::peer_handler_loop(std::shared_ptr<PeerInfo> peer) {
    // ... handshake ...

    // Message receive loop
    while (!shutdown_requested_) {
        auto peer_locked = peer_weak.lock();
        if (!peer_locked || !peer_locked->is_connected) break;

        int socket_fd = peer_locked->socket_fd;
        peer_locked.reset();

        // ❌ BLOCKING RECEIVE (no timeout)
        auto message = receive_message(socket_fd);
        if (!message) break;

        process_message(peer_key, *message);
    }

    cleanup_peer(peer_key);
}
```

### receive_message() Implementation
```cpp
std::unique_ptr<P2PMessage> P2PManager::receive_message(int socket_fd) {
    std::vector<uint8_t> header(24);
    size_t total_received = 0;

    while (total_received < 24) {
        // ❌ BLOCKING recv() with no timeout
        int received = recv(socket_fd,
                           reinterpret_cast<char*>(header.data() + total_received),
                           24 - total_received, 0);

        if (received <= 0) return nullptr;
        total_received += received;
    }

    // ... read payload (also blocking) ...
}
```

### Socket Configuration
```cpp
// From connect_to_peer() and handle_incoming_connection()
set_socket_send_timeout(socket_fd, SEND_TIMEOUT_SEC);  // ✅ Send timeout set

// ❌ NO receive timeout set (SO_RCVTIMEO missing)
```

### TS3 Compliance Matrix

| Property | Status | Evidence |
|----------|--------|----------|
| **TS3.1: Wait Interruptibility** | ❌ **FAIL** | `recv()` blocks until message arrives |
| **TS3.3: Bounded Wait Timeout** | ❌ **FAIL** | No timeout on `recv()` |
| **TS3.4: Shutdown Responsiveness** | ❌ **FAIL** | Thread blocks indefinitely |
| **TS3.5: No Livelock** | ✅ PASS | Blocks (not spinning) |

### Violation Details

**TS3.1 Violation:**
```
Property: shutdown_requested_ = true ⇒ T wakes within Δt_wakeup
Status: VIOLATED

Reason: recv() blocks until data arrives OR peer disconnects
  - If peer is connected but idle, recv() blocks indefinitely
  - Shutdown signal cannot wake recv()
  - Thread only exits when peer sends message or disconnects
```

**TS3.3 Violation:**
```
Property: ∀ wait_for(cv, timeout): timeout < MAX_WAIT_TIMEOUT
Status: VIOLATED

Reason: recv() has no timeout
  - SO_RCVTIMEO not set on sockets
  - recv() can block forever waiting for data
```

**TS3.4 Violation:**
```
Property: stop() invoked at t₀ ⇒ T exits by t₀ + 5s
Status: VIOLATED

Worst Case: INDEFINITE delay
  - 8 peer connections established
  - User calls stop()
  - All 8 peer_handler_loop threads blocked in recv()
  - Threads don't wake up until peers send messages
  - If peers are idle → shutdown hangs indefinitely
```

### Impact Assessment

**User Impact:**
- Shutdown can **hang indefinitely**
- `dinero-cli stop` may never complete
- User must SIGKILL (unclean shutdown)

**Production Impact:**
- Graceful shutdown impossible if peers idle
- Init system timeouts → SIGKILL
- Unclean shutdown → potential data corruption

**Multiplier Effect:**
- **N peer connections = N blocked threads**
- With max_peers = 8, shutdown needs 8 peers to send messages
- Probability of indefinite hang increases with peer count

**Severity:** **HIGH-CRITICAL**

### Risk Assessment
**Risk:** CRITICAL ❌

**Reasoning:**
- Indefinite blocking (not just slow)
- Affects **every peer connection** (N threads)
- No workaround for user (cannot force shutdown)
- Violates all shutdown-related TS3 properties

---

## Summary of Violations

### TS3.1: Wait Interruptibility

| Loop | Status | Max Delay to Wake |
|------|--------|-------------------|
| listen_loop() | ✅ PASS | 1 second |
| connection_manager_loop() | ✅ PASS | 10 seconds |
| outbox_loop() | ✅ PASS | 100 milliseconds |
| **keepalive_loop()** | ❌ **FAIL** | **30 seconds** |
| **peer_handler_loop()** | ❌ **FAIL** | **INDEFINITE** |

### TS3.3: Bounded Wait Timeouts

| Loop | Timeout | Bounded? |
|------|---------|----------|
| listen_loop() | 1s | ✅ YES |
| connection_manager_loop() | 10s | ✅ YES |
| outbox_loop() | 100ms | ✅ YES |
| keepalive_loop() | 30s | ✅ YES |
| **peer_handler_loop()** | **NONE** | ❌ **NO** |

### TS3.4: Shutdown Responsiveness

**Target:** All threads exit within 5 seconds of `stop()` call

| Loop | Max Shutdown Delay | Compliant? |
|------|-------------------|------------|
| listen_loop() | 1s | ✅ YES |
| connection_manager_loop() | 10s | ⚠️ MARGINAL |
| outbox_loop() | 100ms | ✅ YES |
| **keepalive_loop()** | **30s** | ❌ **NO** |
| **peer_handler_loop()** | **∞** | ❌ **NO** |

**Worst-Case Shutdown Time:**
```
Scenario: User calls stop() at worst moment

Thread                   | Delay
-------------------------|-------
listen_loop()            | 1s
connection_manager_loop()| 10s
outbox_loop()            | 0.1s
keepalive_loop()         | 30s  ❌
peer_handler_loop(s)     | ∞    ❌ (blocks until peer sends message)

Total: INDEFINITE (bounded by TCP keepalive timeout ~2 hours)
```

---

## TS3 Compliance Status

| TS3 Property | Status | Violations |
|--------------|--------|------------|
| **TS3.1: Wait Interruptibility** | ❌ **FAIL** | keepalive_loop, peer_handler_loop |
| **TS3.2: Work Queue Fairness** | ✅ PASS | All queues FIFO |
| **TS3.3: Bounded Wait Timeouts** | ❌ **FAIL** | peer_handler_loop |
| **TS3.4: Shutdown Responsiveness** | ❌ **FAIL** | keepalive_loop, peer_handler_loop |
| **TS3.5: No Livelock** | ✅ PASS | No spinning loops |

**Overall TS3 Status:** ❌ **NOT COMPLIANT**

**Violations:** 2 loops (keepalive, peer_handler)

---

## Recommended Fixes

### Fix 1: keepalive_loop() — Use Condition Variable

**Current Code:**
```cpp
void P2PManager::keepalive_loop() {
    while (!shutdown_requested_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));  // ❌
        if (shutdown_requested_) break;
        // ... send pings ...
    }
}
```

**Fixed Code:**
```cpp
void P2PManager::keepalive_loop() {
    std::mutex keepalive_mutex;
    std::condition_variable keepalive_cv;

    while (!shutdown_requested_) {
        {
            std::unique_lock<std::mutex> lock(keepalive_mutex);

            // ✅ Interruptible wait with 30s timeout
            keepalive_cv.wait_for(lock, std::chrono::seconds(30),
                [this]{ return shutdown_requested_.load(); });
        }

        if (shutdown_requested_) break;

        // ... send pings ...
    }
}

// In stop():
void P2PManager::stop() {
    shutdown_requested_ = true;
    keepalive_cv.notify_all();  // ✅ Wake keepalive thread immediately
    // ...
}
```

**Benefits:**
- Shutdown delay: 30s → <100ms
- TS3.1 compliance: ✅ (interruptible)
- TS3.4 compliance: ✅ (responsive)

**Pattern:** Same as outbox_loop() (proven compliant)

---

### Fix 2: peer_handler_loop() — Set SO_RCVTIMEO

**Option A: Socket-Level Timeout (Recommended)**

```cpp
void P2PManager::set_socket_recv_timeout(int socket_fd, int seconds) {
#ifdef _WIN32
    DWORD timeout = seconds * 1000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

// In connect_to_peer() and handle_incoming_connection():
set_socket_send_timeout(socket_fd, SEND_TIMEOUT_SEC);
set_socket_recv_timeout(socket_fd, 30);  // ✅ 30s receive timeout
```

**receive_message() Update:**
```cpp
std::unique_ptr<P2PMessage> P2PManager::receive_message(int socket_fd) {
    // ... existing code ...

    while (total_received < 24) {
        int received = recv(socket_fd, ...);

        if (received < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // ✅ Timeout occurred, check shutdown
                if (shutdown_requested_) return nullptr;
                continue;  // Retry recv
            }
            return nullptr;  // Real error
        }
        if (received == 0) return nullptr;  // Peer closed

        total_received += received;
    }
    // ...
}
```

**Benefits:**
- Shutdown delay: ∞ → 30s (bounded)
- TS3.1 compliance: ✅ (wakes every 30s to check shutdown)
- TS3.3 compliance: ✅ (bounded timeout)
- TS3.4 compliance: ⚠️ MARGINAL (30s still > 5s target)

**Tradeoff:**
- Adds spurious wakeups every 30s when idle
- But necessary for shutdown responsiveness

---

**Option B: select() Before recv() (Better Shutdown)**

```cpp
std::unique_ptr<P2PMessage> P2PManager::receive_message(int socket_fd) {
    std::vector<uint8_t> header(24);
    size_t total_received = 0;

    while (total_received < 24) {
        // ✅ Check for data with 1s timeout
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);

        if (activity < 0) return nullptr;  // Error
        if (activity == 0) {
            // ✅ Timeout - check shutdown
            if (shutdown_requested_) return nullptr;
            continue;  // Retry select
        }

        // Data available, non-blocking recv
        int received = recv(socket_fd, ...);
        if (received <= 0) return nullptr;
        total_received += received;
    }
    // ...
}
```

**Benefits:**
- Shutdown delay: ∞ → 1s (excellent)
- TS3.1 compliance: ✅
- TS3.3 compliance: ✅
- TS3.4 compliance: ✅ (1s < 5s target)

**Recommended:** Option B (better shutdown responsiveness)

---

## Next Steps

### Step 1: Fix keepalive_loop() ✅
- Add condition variable + mutex
- Replace sleep_for with wait_for
- Update stop() to notify condition variable
- **Estimated Effort:** 30 minutes

### Step 2: Fix peer_handler_loop() ✅
- Choose Option A (SO_RCVTIMEO) or Option B (select)
- Implement timeout mechanism
- Update receive_message() to check shutdown
- **Estimated Effort:** 1-2 hours

### Step 3: Test Fixes ✅
- Run TS3 test suite (should still pass - mocks already compliant)
- Add production shutdown test
  - Start P2PManager with 8 idle peers
  - Call stop()
  - Measure shutdown duration
  - Assert: duration < 5 seconds
- **Estimated Effort:** 30 minutes

### Step 4: Optional - Fix connection_manager_loop() ⚠️
- Reduce sleep from 10s to 1-2s OR
- Convert to condition variable pattern
- **Priority:** LOW (marginal, not critical)
- **Estimated Effort:** 15 minutes

### Step 5: Documentation ✅
- Update TS3 compliance report
- Tag completion: `v1.3.7-ring3-phase4e-ts3`
- **Estimated Effort:** 15 minutes

**Total Estimated Effort:** 3-4 hours

---

## Testing Strategy

### Unit Tests (Already Passing)
```bash
./build/test_thread_safety_ts3
# Expected: 8/8 tests PASS (mock tests already TS3-compliant)
```

### Integration Test (To Be Added)
```cpp
TEST(TS3Integration, ProductionShutdownResponsiveness) {
    P2PManager manager(20998);
    manager.start();

    // Establish 8 idle peer connections
    for (int i = 0; i < 8; i++) {
        manager.connect_to_peer("seed" + std::to_string(i), 20000 + i);
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));  // Let connections settle

    // Measure shutdown time
    auto start = std::chrono::steady_clock::now();
    manager.stop();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start
    );

    // TS3.4: Shutdown < 5 seconds
    EXPECT_LT(duration.count(), 5);
}
```

---

## Risk Assessment After Fixes

| Loop | Before Fix | After Fix |
|------|-----------|-----------|
| listen_loop() | ✅ COMPLIANT | ✅ COMPLIANT |
| connection_manager_loop() | ⚠️ MARGINAL | ⚠️ MARGINAL (optional fix) |
| outbox_loop() | ✅ COMPLIANT | ✅ COMPLIANT |
| keepalive_loop() | ❌ 30s delay | ✅ <100ms delay |
| peer_handler_loop() | ❌ ∞ delay | ✅ 1s delay |

**Overall Risk:**
- Before: CRITICAL ❌ (indefinite shutdown hang)
- After: LOW ✅ (shutdown < 5s guaranteed)

---

## Conclusion

**Production P2PManager violates TS3** due to:
1. Uninterruptible 30s sleep in keepalive_loop()
2. Indefinite blocking recv() in peer_handler_loop()

**Impact:** Shutdown can hang indefinitely (user must SIGKILL)

**Fixes:** Both violations have straightforward solutions (condition variable + recv timeout)

**Effort:** 3-4 hours to implement and test

**Next Action:** Implement fixes before proceeding to TS3 compliance report.

---

_Ring 3 Phase 4e — TS3 Production Audit Complete_
_Status: VIOLATIONS FOUND (fixes required)_
_Date: 2026-01-02_
