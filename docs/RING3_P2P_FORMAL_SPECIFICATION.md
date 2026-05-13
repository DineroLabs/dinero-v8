# Ring 3: P2P Protocol Formal Specification

**Status**: DESIGN PHASE
**Created**: 2026-01-02
**Prerequisite**: Ring 1 (Consensus) + Ring 2 (Wallet) complete
**Baseline**: v1.0.12

---

## Overview

Ring 3 provides **mathematical proofs** of P2P protocol correctness using property-based testing. Unlike Ring 1 (consensus invariants) and Ring 2 (wallet determinism), Ring 3 proves **network protocol safety** and **concurrent correctness**.

## Design Philosophy

- **State Machine Formalism**: Every connection is a finite state machine with proven transitions
- **Concurrency Correctness**: Thread safety proven through property tests, not hoped for
- **Protocol Safety**: Invalid messages provably rejected, valid messages provably handled
- **Resource Safety**: Memory leaks impossible, use-after-free impossible
- **Deterministic Testing**: P2P behavior reproducible through controlled peer simulation

---

## Ring 3 Core Invariants (3 Fundamental Guarantees)

### Invariant 1: Connection Lifecycle Safety
```
∀ connection C:
  1. state(C) ∈ {DISCONNECTED, CONNECTING, HANDSHAKING, ESTABLISHED, DISCONNECTING}
  2. state transitions are monotonic (no ESTABLISHED → CONNECTING)
  3. DISCONNECTED is absorbing (terminal state)
  4. resources freed exactly once (on DISCONNECTED entry)
  5. no message sent in DISCONNECTED or DISCONNECTING state
```

### Invariant 2: Message Ordering Safety
```
∀ connection C, message M:
  1. send(VERSION) requires state(C) = CONNECTING
  2. send(VERACK) requires recv(VERSION) ∧ state(C) = HANDSHAKING
  3. send(M) where M ∉ {VERSION, VERACK} requires state(C) = ESTABLISHED
  4. messages delivered in send order (FIFO per connection)
  5. no message processed after disconnect initiated
```

### Invariant 3: Resource Lifecycle Safety
```
∀ connection C:
  1. exactly one thread owns C at any time (ownership model)
  2. shared access requires mutex (read/write synchronization)
  3. C freed only when refcount = 0 (no dangling pointers)
  4. send buffer bounded (no unbounded memory growth)
  5. disconnect always completes (no resource leaks)
```

---

## Connection State Machine (Formal Definition)

### States

```
DISCONNECTED    - No connection exists, all resources freed
CONNECTING      - TCP connection initiated, waiting for socket ready
HANDSHAKING     - VERSION/VERACK exchange in progress
ESTABLISHED     - Handshake complete, normal message flow
DISCONNECTING   - Graceful shutdown in progress, no new messages
```

### Valid Transitions

```
DISCONNECTED → CONNECTING
  Trigger: connect(peer_addr)
  Precondition: none
  Postcondition: socket allocated, send buffer created

CONNECTING → HANDSHAKING
  Trigger: socket connected event
  Precondition: socket writable
  Postcondition: VERSION message sent

HANDSHAKING → ESTABLISHED
  Trigger: recv(VERACK) ∧ sent(VERACK)
  Precondition: VERSION exchanged both directions
  Postcondition: peer validated, message queues active

{CONNECTING, HANDSHAKING, ESTABLISHED} → DISCONNECTING
  Trigger: disconnect_requested() ∨ protocol_violation() ∨ timeout()
  Precondition: state ≠ DISCONNECTED
  Postcondition: no new messages accepted

DISCONNECTING → DISCONNECTED
  Trigger: send_buffer_empty() ∧ socket_closed()
  Precondition: all pending messages flushed
  Postcondition: all resources freed
```

### Invalid Transitions (Must Be Rejected)

```
❌ ESTABLISHED → CONNECTING      (connection cannot restart)
❌ DISCONNECTED → HANDSHAKING     (must go through CONNECTING)
❌ DISCONNECTING → ESTABLISHED    (disconnect is irreversible)
❌ Any state → DISCONNECTING if already DISCONNECTED
```

---

## Protocol Message Invariants

### Handshake Protocol (Bitcoin-Compatible)

```
Initiator (Outbound Connection):
  1. Send VERSION immediately on connect
  2. Wait for peer VERSION
  3. Validate peer VERSION (version ≥ MIN_PROTO_VERSION, correct magic bytes)
  4. Send VERACK
  5. Wait for peer VERACK
  6. Transition to ESTABLISHED

Responder (Inbound Connection):
  1. Wait for peer VERSION
  2. Validate peer VERSION
  3. Send VERSION reply
  4. Send VERACK
  5. Wait for peer VERACK
  6. Transition to ESTABLISHED

Invariants:
  ∀ established connection C:
    - exactly 1 VERSION sent in each direction
    - exactly 1 VERACK sent in each direction
    - VERSION precedes VERACK in both directions
    - no other messages sent before both VERACKs received
```

### Message Processing Order

```
∀ connection C, messages [M₁, M₂, ..., Mₙ]:
  1. send(Mᵢ) → send(Mⱼ) where i < j implies Mᵢ delivered before Mⱼ
  2. recv(Mᵢ) triggers handler(Mᵢ) before recv(Mⱼ) where i < j
  3. handler(Mᵢ) completes before handler(Mⱼ) starts (serialized processing)
  4. disconnect() → no further recv() callbacks (cleanup is atomic)
```

### Ping/Pong Liveness Protocol

```
∀ established connection C:
  1. send(PING, nonce=N) every PING_INTERVAL seconds
  2. recv(PING, nonce=N) → send(PONG, nonce=N) immediately
  3. recv(PONG, nonce=N) → validate N matches outstanding PING
  4. timeout(PONG) after PONG_TIMEOUT → increment stale_count
  5. stale_count > MAX_STALE_PINGS → disconnect(reason="timeout")

Invariants:
  - at most 1 outstanding PING per connection (no PING spam)
  - PONG nonce must exactly match PING nonce (replay protection)
  - PONG without matching PING → protocol violation → disconnect
```

---

## Concurrency Model (Thread Safety)

### Thread Ownership Model

```
Connection Manager Thread (single):
  - Owns all connection state machines
  - Receives events: socket_ready, message_received, timeout
  - Executes state transitions atomically
  - Dispatches messages to handlers

Message Handler Threads (pool):
  - Process application-level messages (INV, GETDATA, TX, BLOCK)
  - Read-only access to connection state
  - Cannot modify connection state directly
  - Submit responses via send queue

Network I/O Thread (per connection):
  - Reads from socket → enqueues messages for Connection Manager
  - Writes from send queue → socket
  - Detects disconnect → notifies Connection Manager
  - No access to application state
```

### Synchronization Invariants

```
∀ connection C:
  1. state(C) modified only by Connection Manager thread (single writer)
  2. state(C) read by any thread via atomic load (lock-free reads)
  3. send_queue(C) is lock-free MPSC queue (multiple producers, single consumer)
  4. recv_queue(C) is lock-free SPMC queue (single producer, multiple consumers)
  5. disconnect(C) is idempotent (multiple calls safe)

Deadlock Prevention:
  - No nested locks (lock ordering unnecessary)
  - Timeouts on all blocking operations
  - State machine guarantees forward progress
```

### Resource Ownership

```
Connection Object Lifecycle:
  1. Allocated in CONNECTING state (ref_count = 1)
  2. Reference acquired by each I/O operation (ref_count++)
  3. Reference released on I/O completion (ref_count--)
  4. Freed when ref_count reaches 0 AND state = DISCONNECTED
  5. Smart pointers enforce this (std::shared_ptr<Connection>)

Invariants:
  ∀ connection C:
    - C accessed only through shared_ptr (no raw pointers)
    - weak_ptr used for non-owning references (e.g., timeout callbacks)
    - ref_count > 0 → C memory valid
    - ref_count = 0 → C freed immediately (RAII)
```

---

## Property-Based Tests (Ring 3 Proof System)

### Test 1: Connection Lifecycle Properties

**Priority**: FIRST (foundation for all other tests)

**Properties to Prove**:
```
P1.1: State Monotonicity
  ∀ connection C, time t₁ < t₂:
    state(C, t₁) = CONNECTING → state(C, t₂) ∈ {CONNECTING, HANDSHAKING, ESTABLISHED, DISCONNECTING, DISCONNECTED}
    state(C, t₁) = ESTABLISHED → state(C, t₂) ∈ {ESTABLISHED, DISCONNECTING, DISCONNECTED}
    state(C, t₁) = DISCONNECTED → state(C, t₂) = DISCONNECTED

P1.2: Resource Cleanup Guarantee
  ∀ connection C:
    disconnect(C) → eventually state(C) = DISCONNECTED ∧ resources_freed(C)

P1.3: No Dangling Pointers
  ∀ connection C:
    ref_count(C) = 0 → no code path accesses C

P1.4: Disconnect Idempotence
  ∀ connection C:
    disconnect(C); disconnect(C); disconnect(C) ≡ disconnect(C)
```

**Property Tests**:
- Generate 10,000 random connection sequences (connect, send, disconnect)
- Verify state transitions always valid
- Verify resources always freed
- Verify no segfaults, no memory leaks (valgrind/asan)

**Pass Criteria**:
- ✅ All 10,000 random sequences execute without crash
- ✅ All state transitions obey monotonicity
- ✅ All resources freed (verified by ASAN/valgrind)

---

### Test 2: Handshake Protocol Properties

**Priority**: SECOND (enables message flow)

**Properties to Prove**:
```
P2.1: Handshake Completion
  ∀ connection C initiated:
    valid_peer(C) → eventually state(C) = ESTABLISHED ∨ state(C) = DISCONNECTED

P2.2: Handshake Ordering
  ∀ established connection C:
    send_log(C) = [VERSION, VERACK, ...] (no messages before VERSION)
    recv_log(C) = [VERSION, VERACK, ...] (no messages before VERSION)

P2.3: Protocol Violation Rejection
  ∀ connection C:
    recv(M) where M ∉ {VERSION, VERACK} ∧ state(C) = HANDSHAKING
      → disconnect(C, reason="protocol_violation")

P2.4: Version Validation
  ∀ connection C:
    recv(VERSION) where version < MIN_PROTO_VERSION → reject(C)
    recv(VERSION) where magic ≠ NETWORK_MAGIC → reject(C)
```

**Property Tests**:
- Generate 1,000 valid handshake sequences
- Generate 1,000 invalid handshake sequences (wrong order, bad version)
- Verify valid sequences reach ESTABLISHED
- Verify invalid sequences disconnect with protocol_violation

**Pass Criteria**:
- ✅ 1,000/1,000 valid handshakes succeed
- ✅ 1,000/1,000 invalid handshakes rejected
- ✅ No handshake takes longer than HANDSHAKE_TIMEOUT

---

### Test 3: Message Ordering Properties

**Priority**: THIRD (ensures protocol correctness)

**Properties to Prove**:
```
P3.1: FIFO Delivery
  ∀ connection C, messages [M₁, M₂, ..., Mₙ]:
    send_order = [M₁, M₂, ..., Mₙ] → recv_order = [M₁, M₂, ..., Mₙ]

P3.2: No Message Reordering
  ∀ connection C:
    send(Mᵢ) happens-before send(Mⱼ) → recv(Mᵢ) happens-before recv(Mⱼ)

P3.3: No Message Loss (Before Disconnect)
  ∀ connection C in ESTABLISHED state:
    send(M) → eventually recv(M) ∨ disconnect(C)

P3.4: No Messages After Disconnect
  ∀ connection C:
    state(C) = DISCONNECTING → no new recv() callbacks
```

**Property Tests**:
- Generate 100 connections, send 1,000 messages each
- Verify all messages delivered in order
- Inject random disconnects, verify no messages lost before disconnect
- Verify no callbacks after disconnect initiated

**Pass Criteria**:
- ✅ 100,000 messages delivered (100 conns × 1,000 msgs)
- ✅ 0 reordering violations
- ✅ 0 callbacks after disconnect

---

### Test 4: Ping/Pong Liveness Properties

**Priority**: FOURTH (peer liveness detection)

**Properties to Prove**:
```
P4.1: Ping Periodicity
  ∀ established connection C:
    time_since_last_ping(C) ≥ PING_INTERVAL → send(PING) within ε seconds

P4.2: Pong Response
  ∀ connection C:
    recv(PING, nonce=N) → send(PONG, nonce=N) within PONG_RESPONSE_TIME

P4.3: Nonce Matching
  ∀ connection C:
    recv(PONG, nonce=N) → ∃ outstanding PING with nonce=N

P4.4: Timeout Detection
  ∀ connection C:
    no PONG received for PONG_TIMEOUT → stale_count++
    stale_count > MAX_STALE_PINGS → disconnect(C)
```

**Property Tests**:
- Simulate 100 connections for 5 minutes each
- Verify PING sent every PING_INTERVAL ± tolerance
- Verify all PONGs match outstanding PINGs
- Inject stale peers (never respond to PING), verify disconnect

**Pass Criteria**:
- ✅ PING timing variance < 10% of PING_INTERVAL
- ✅ 100% of PONGs match outstanding PINGs
- ✅ 100% of stale peers disconnected within timeout window

---

### Test 5: Concurrent Safety Properties

**Priority**: FIFTH (thread safety proof)

**Properties to Prove**:
```
P5.1: No Data Races
  ∀ connection C, threads T₁, T₂:
    write(C, field) by T₁ ∧ access(C, field) by T₂
      → access is synchronized (mutex or atomic)

P5.2: No Deadlocks
  ∀ system state S:
    ∃ thread T in S that can make progress (liveness)

P5.3: Memory Safety
  ∀ connection C:
    no use-after-free (verified by ASAN)
    no double-free (verified by ASAN)
    no memory leaks (verified by valgrind)

P5.4: Send Queue Bounded
  ∀ connection C:
    |send_queue(C)| ≤ MAX_SEND_QUEUE_SIZE
    send() blocks if queue full (backpressure)
```

**Property Tests**:
- Run 1,000 connections with 100 threads hammering send/recv
- Verify no data races (TSAN - Thread Sanitizer)
- Verify no deadlocks (watchdog timer)
- Verify no memory errors (ASAN - Address Sanitizer)
- Stress test send queue (send faster than network can drain)

**Pass Criteria**:
- ✅ 0 data races (TSAN clean)
- ✅ 0 deadlocks (all threads make progress)
- ✅ 0 memory errors (ASAN clean)
- ✅ Send queue never exceeds MAX_SEND_QUEUE_SIZE

---

## Implementation Strategy

### Phase 1: Deterministic Peer Simulator (1-2 weeks)

**Goal**: Build test infrastructure that replaces real network I/O with controlled simulation.

**Components**:

1. **MockSocket**: Simulated TCP socket with controlled latency/packet loss
   ```cpp
   class MockSocket {
       void inject_latency(milliseconds delay);
       void inject_packet_loss(double probability);
       void inject_disconnect();
       std::vector<uint8_t> read_next_message();
       void write_message(const std::vector<uint8_t>& msg);
   };
   ```

2. **PeerSimulator**: Simulated peer that responds to messages
   ```cpp
   class PeerSimulator {
       void on_version(const VersionMessage& msg) {
           send_version_reply();
           send_verack();
       }
       void on_ping(const PingMessage& msg) {
           send_pong(msg.nonce);
       }
       void set_behavior(PeerBehavior b); // HONEST, STALE, MALICIOUS
   };
   ```

3. **DeterministicScheduler**: Controlled thread scheduling for reproducibility
   ```cpp
   class DeterministicScheduler {
       void set_seed(uint64_t seed); // Fixed seed for reproducibility
       void schedule(std::function<void()> task);
       void run_until_idle(); // Process all pending events
   };
   ```

**Deliverable**: Can simulate 1,000 peer connections without real network I/O.

---

### Phase 2: Property Test Framework (1 week)

**Goal**: Build property-based testing framework similar to QuickCheck/Hypothesis.

**Components**:

1. **ConnectionSequenceGenerator**: Generate random connection event sequences
   ```cpp
   struct Event {
       enum Type { CONNECT, SEND_MSG, RECV_MSG, DISCONNECT, TIMEOUT };
       Type type;
       uint64_t timestamp_ms;
       std::optional<Message> message;
   };

   std::vector<Event> generate_random_sequence(RNG& rng, size_t num_events);
   ```

2. **PropertyAssertion**: DSL for expressing properties
   ```cpp
   PropertyTest("Connection lifecycle monotonicity")
       .forAll([](Connection& c) {
           State s1 = c.state();
           c.process_events(random_events());
           State s2 = c.state();
           return is_valid_transition(s1, s2);
       })
       .repeat(10000);
   ```

3. **InvariantChecker**: Verify invariants at each step
   ```cpp
   class InvariantChecker {
       void check_state_machine(const Connection& c);
       void check_resource_ownership(const Connection& c);
       void check_message_ordering(const Connection& c);
   };
   ```

**Deliverable**: Can run 10,000+ property tests with random inputs.

---

### Phase 3: Implement Tests 1-5 (2-3 weeks)

**Goal**: Implement all 5 property test suites.

**Test File Structure**:
```
tests/p2p/test_p2p_formal_verification.cpp
  - Test 1: Connection Lifecycle (4 properties, 10k random sequences)
  - Test 2: Handshake Protocol (4 properties, 2k handshake sequences)
  - Test 3: Message Ordering (4 properties, 100k messages)
  - Test 4: Ping/Pong Liveness (4 properties, 100 peers × 5 min)
  - Test 5: Concurrent Safety (4 properties, 1k connections × 100 threads)
```

**Deliverable**: All tests written, most failing (P2P code has bugs from RING3_P2P_KNOWN_ISSUES.md).

---

### Phase 4: Fix P2P Implementation (2-4 weeks)

**Goal**: Fix all P2P bugs until all property tests pass.

**Known Bugs to Fix** (from RING3_P2P_KNOWN_ISSUES.md):
1. P2P handshake segfault (lifecycle bug)
2. P2P ping/pong segfault (lifecycle bug)
3. Mempool ancestor/descendant segfault (threading bug)

**Fix Strategy**:
- Fix one property at a time
- Run full test suite after each fix (prevent regressions)
- Use ASAN/TSAN/valgrind to catch memory/threading bugs
- Prove each fix with property test passing

**Deliverable**: All 5 property test suites passing (20 properties × 1,000+ iterations).

---

### Phase 5: Stress Testing & Fuzzing (1-2 weeks)

**Goal**: Prove P2P layer robust under adversarial conditions.

**Stress Tests**:
1. **Connection Churn**: 10,000 rapid connect/disconnect cycles
2. **Message Flood**: Send 1M messages through 100 connections
3. **Malicious Peers**: Inject invalid messages, wrong nonces, protocol violations
4. **Network Failures**: Random packet loss, high latency, connection drops
5. **Resource Exhaustion**: Max connections, full send queues, OOM conditions

**Fuzzing**:
- Fuzz handshake messages (random VERSION fields)
- Fuzz message headers (invalid lengths, wrong checksums)
- Fuzz message ordering (send BLOCK before GETDATA)

**Deliverable**: 0 crashes, 0 memory leaks, 0 data races under all stress conditions.

---

## Success Criteria (Ring 3 Complete)

When all 5 Ring 3 tests pass with 10,000+ random inputs each:
- ✅ Connection lifecycle proven safe (monotonic state machine)
- ✅ Handshake protocol proven correct (Bitcoin-compatible)
- ✅ Message ordering proven FIFO (no reordering/loss)
- ✅ Ping/pong liveness proven (stale peer detection)
- ✅ Concurrent safety proven (no data races, deadlocks, memory errors)
- ✅ All known P2P segfaults fixed (from RING3_P2P_KNOWN_ISSUES.md)

---

## Comparison: Ring 1 vs Ring 2 vs Ring 3

| Aspect | Ring 1 (Consensus) | Ring 2 (Wallet) | Ring 3 (P2P) |
|--------|-------------------|----------------|--------------|
| **Goal** | Prove consensus rules correct | Prove wallet state correct | Prove protocol safe |
| **Method** | Property-based (supply, UTXO, chain) | Fixture-based (restore, persist) | Property-based (state machine, concurrency) |
| **Scope** | Economic invariants | Deterministic wallet behavior | Network protocol safety |
| **Tests** | 22 property tests, 100k+ random heights | 9 wallet correctness tests | 20 property tests, 10k+ random sequences |
| **Priority** | MANDATORY (chain split risk) | MANDATORY (fund loss risk) | MANDATORY (crash/exploit risk) |

---

## Build & Run

```bash
# Enable Ring 3 tests
cmake .. -DENABLE_P2P_FORMAL_VERIFICATION=ON

# Build Ring 3 test suite
cmake --build build --target test_p2p_formal_verification

# Run Ring 3 tests (expects failures initially)
./build/test_p2p_formal_verification

# Run with sanitizers (memory safety)
cmake .. -DENABLE_P2P_FORMAL_VERIFICATION=ON -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
./build/test_p2p_formal_verification

# Run with thread sanitizer (concurrency bugs)
cmake .. -DENABLE_P2P_FORMAL_VERIFICATION=ON -DENABLE_TSAN=ON
./build/test_p2p_formal_verification
```

**Expected Output (After Phase 4 Complete)**:
```
[Test 1] Connection Lifecycle Properties
  [✓] P1.1: State Monotonicity (10,000 random sequences)
  [✓] P1.2: Resource Cleanup (10,000 disconnects, 0 leaks)
  [✓] P1.3: No Dangling Pointers (ASAN clean)
  [✓] P1.4: Disconnect Idempotence (1,000 tests)

[Test 2] Handshake Protocol Properties
  [✓] P2.1: Handshake Completion (1,000 valid handshakes)
  [✓] P2.2: Handshake Ordering (1,000 sequences verified)
  [✓] P2.3: Protocol Violation Rejection (1,000 invalid handshakes rejected)
  [✓] P2.4: Version Validation (1,000 tests)

[Test 3] Message Ordering Properties
  [✓] P3.1: FIFO Delivery (100,000 messages, 0 reorders)
  [✓] P3.2: No Message Reordering (100 connections tested)
  [✓] P3.3: No Message Loss (99.99% delivery before disconnect)
  [✓] P3.4: No Messages After Disconnect (1,000 tests)

[Test 4] Ping/Pong Liveness Properties
  [✓] P4.1: Ping Periodicity (100 peers × 5 min, <5% variance)
  [✓] P4.2: Pong Response (10,000 pings, 100% response)
  [✓] P4.3: Nonce Matching (10,000 pongs, 100% match)
  [✓] P4.4: Timeout Detection (100 stale peers, 100% disconnected)

[Test 5] Concurrent Safety Properties
  [✓] P5.1: No Data Races (TSAN clean, 1,000 connections)
  [✓] P5.2: No Deadlocks (100 threads, 0 hangs)
  [✓] P5.3: Memory Safety (ASAN clean, 0 leaks)
  [✓] P5.4: Send Queue Bounded (1,000 tests, backpressure working)

Ring 3 Complete: All P2P protocol invariants proven ✅
```

---

## Known Challenges & Mitigations

### Challenge 1: Non-Deterministic Concurrency

**Problem**: Real multi-threading is non-deterministic (thread scheduling, race conditions).

**Mitigation**:
- Use DeterministicScheduler in tests (controlled event ordering)
- Use Thread Sanitizer (TSAN) to detect data races
- Property tests with random thread interleavings (explore state space)

### Challenge 2: Network I/O Simulation

**Problem**: Real network I/O is slow and flaky (can't run 10,000 connections in CI).

**Mitigation**:
- MockSocket replaces real TCP (in-memory queues)
- PeerSimulator replaces real peers (scripted responses)
- Tests run in milliseconds, not seconds

### Challenge 3: Resource Leak Detection

**Problem**: Memory leaks only show up over time (10,000 connections might leak 1MB each = 10GB).

**Mitigation**:
- Use AddressSanitizer (ASAN) to detect leaks immediately
- Use valgrind for leak reports
- Property test: allocate 10,000 connections → disconnect all → verify 0 bytes leaked

### Challenge 4: Deadlock Detection

**Problem**: Deadlocks are hard to detect (tests just hang forever).

**Mitigation**:
- Watchdog timer in tests (fail if no progress for 10 seconds)
- DeterministicScheduler detects cycles (no thread can proceed)
- Property test: verify all threads eventually terminate

---

## Security Considerations

### DoS Attack Resistance

```
∀ malicious peer P:
  1. P cannot exhaust memory (connection limit, send queue limit)
  2. P cannot exhaust CPU (message rate limiting)
  3. P cannot cause deadlock (timeouts on all operations)
  4. P cannot cause crash (all inputs validated)
```

**Property Tests**:
- Generate 1,000 malicious peers (send junk, spam messages)
- Verify node stays responsive
- Verify memory usage bounded

### Protocol Compliance

```
∀ connection C to Bitcoin Core:
  1. Handshake compatible (VERSION/VERACK format)
  2. Message format compatible (serialization)
  3. Ping/pong compatible (nonce format)
```

**Integration Tests**:
- Connect to real Bitcoin Core node
- Verify handshake succeeds
- Verify can exchange blocks/transactions

---

## Next Steps (After Ring 3 Complete)

Once Ring 3 passes:

1. **Ring 4: Performance Optimization**
   - Prove block relay latency < 100ms
   - Prove mempool propagation < 500ms
   - Prove can handle 10,000 connections

2. **Ring 5: Privacy Protocol**
   - Prove Tor/I2P integration correct
   - Prove no IP address leaks
   - Prove transaction origin privacy

3. **Public Testnet Launch**
   - All rings complete (1, 2, 3)
   - Full node stability proven
   - Ready for external testing

---

## References

- **Ring 1 Design**: `docs/RING1_FORMAL_VERIFICATION_DESIGN.md`
- **Ring 3 Known Issues**: `docs/RING3_P2P_KNOWN_ISSUES.md`
- **Bitcoin P2P Protocol**: https://en.bitcoin.it/wiki/Protocol_documentation
- **QuickCheck**: https://hackage.haskell.org/package/QuickCheck
- **Thread Sanitizer**: https://github.com/google/sanitizers

---

## Document Metadata

- **Created**: 2026-01-02
- **Author**: Claude Sonnet 4.5 (via Claude Code)
- **Purpose**: Formal specification for Ring 3 P2P protocol verification
- **Status**: Design phase - not yet implemented

**Last Updated**: 2026-01-02
