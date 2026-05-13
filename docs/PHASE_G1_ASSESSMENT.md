# Phase G.1: P2P Protocol Verification - Code Assessment

**Date:** 2025-12-17
**Status:** Assessment Complete - Ready for Testing
**Goal:** Verify existing P2P protocol implementation

---

## Existing Code Assessment

### ✅ **P2P Infrastructure (COMPLETE)**

**Files Found:**
```
src/daemon/p2p_manager.h            - Core P2P manager interface
src/daemon/p2p_manager.cpp          - P2P manager implementation
src/daemon/peer_connection.cpp      - Peer connection handling
src/daemon/p2p/messages.cpp         - Message serialization
src/p2p/addrman.cpp          - Address manager
src/daemon/peer_scoring.cpp         - Peer reputation system
src/daemon/peer_reputation_db.cpp   - Persistent peer database
src/daemon/network_message_handlers.cpp - Message handlers
```

### P2P Manager Features

**Lifecycle Management:**
- ✅ `start()` / `stop()` - Start/stop P2P networking
- ✅ Multi-threaded architecture (listen, connection manager, keepalive, peer handlers)
- ✅ Graceful shutdown handling

**Peer Management:**
- ✅ Outbound connections (`connect_to_peer()`)
- ✅ Inbound connections (listen loop)
- ✅ Seed node support (`add_seed_node()`)
- ✅ Peer disconnection (`disconnect_peer()`)
- ✅ Persistent peer database (load/save peers)

**Message Protocol:**
- ✅ `version` / `verack` - Handshake
- ✅ `ping` / `pong` - Keepalive
- ✅ `getaddr` / `addr` - Peer discovery
- ✅ `inv` / `getdata` - Block/transaction announcement
- ✅ `getheaders` / `headers` - Headers-first sync
- ✅ `block` - Block transmission

**Message Handling:**
- ✅ Async message sending (outbox queue)
- ✅ Message handlers (callbacks)
- ✅ Broadcast support (to all peers)
- ✅ Checksum validation
- ✅ Message serialization/deserialization

**Network Layer:**
- ✅ TCP socket management
- ✅ Non-blocking I/O
- ✅ Send timeouts
- ✅ IPv4/IPv6 support (IPv4-mapped IPv6)

### Message Format

**Header Structure (24 bytes):**
```
+--------+--------+--------+--------+
| Magic  |     Command     | Length |
| (4)    |       (12)      |  (4)   |
+--------+--------+--------+--------+
| Checksum (4)              | Payload...
+--------+--------+--------+--------+
```

**Magic Bytes:**
- Mainnet: `0xd9b4bef9` (defined in constants)
- Testnet: (separate magic)
- Regtest: (separate magic)

**Commands Supported:**
- `version` - Protocol version and capabilities
- `verack` - Version acknowledgment
- `ping` - Keepalive request
- `pong` - Keepalive response
- `getaddr` - Request peer addresses
- `addr` - Send peer addresses
- `inv` - Inventory announcement
- `getdata` - Request inventory data
- `getheaders` - Request block headers
- `headers` - Send block headers
- `block` - Send full block

### Peer Connection Lifecycle

**Outbound Connection:**
1. `connect_to_peer(address, port)` - Create outbound connection
2. `perform_handshake()` - Send `version`, receive `verack`
3. Peer added to `connected_peers_` map
4. Spawn `peer_handler_loop()` thread
5. Start keepalive (adaptive ping/pong)

**Inbound Connection:**
1. `listen_loop()` accepts incoming connection
2. `handle_incoming_connection()` receives `version`
3. `perform_handshake()` - Send `verack`, then `version`
4. Peer added to `connected_peers_` map
5. Spawn `peer_handler_loop()` thread
6. Start keepalive

**Disconnection:**
1. `disconnect_peer()` or connection error
2. `cleanup_peer()` - Remove from `connected_peers_`
3. Close socket
4. Join peer handler thread
5. Notify `peer_disconnected_handler_`

### Peer Info Tracking

**PeerInfo Structure:**
```cpp
struct PeerInfo {
    std::string address;           // IP address
    uint16_t port;                 // Port number
    std::string user_agent;        // Peer client version
    uint32_t protocol_version;     // Protocol version
    uint32_t best_height;          // Peer's best block height
    uint32_t synced_blocks;        // Blocks synced from peer
    uint64_t bytes_recv;           // Bytes received
    uint64_t bytes_sent;           // Bytes sent
    std::chrono::time_point last_seen;  // Last message time
    bool is_outbound;              // Outbound connection?
    bool is_connected;             // Currently connected?
    int socket_fd;                 // Socket file descriptor

    // Phase C additions:
    int64_t last_seen_unix;        // For persistence
    double avg_latency_ms;         // Ping latency (EMA)
};
```

### Address Manager

**Features:**
- Peer address storage (new/tried buckets)
- Address selection algorithm
- Peer banning logic
- Address serialization (peers.dat)

**Not Assessed Yet:**
- Implementation details
- Test coverage

### Peer Scoring/Reputation

**Features:**
- Reputation scoring system
- Misbehavior tracking
- Automatic banning
- Reputation persistence

**Not Assessed Yet:**
- Scoring algorithm
- Ban thresholds
- Test coverage

---

## What Needs Testing

### G.1.1: P2P Message Protocol ❌

**Tests to Create:**
1. **Version Handshake**
   - Outbound: Send `version` → Receive `verack` → Send `verack`
   - Inbound: Receive `version` → Send `verack` + `version` → Receive `verack`
   - Invalid version (protocol mismatch)
   - Missing verack (timeout)

2. **Ping/Pong Keepalive**
   - Send ping with nonce → Receive pong with same nonce
   - Verify latency tracking (avg_latency_ms)
   - Adaptive keepalive (based on latency)
   - Timeout (peer doesn't respond to ping)

3. **Peer Discovery**
   - Send `getaddr` → Receive `addr` with peer list
   - Verify addr message parsing
   - Test address broadcast (relay addr to other peers)

4. **Block Announcement**
   - Send `inv` (block hash) → Receive `getdata` → Send `block`
   - Verify inventory types (block, tx, etc.)
   - Test duplicate inv suppression

5. **Headers-First Sync**
   - Send `getheaders` (locator) → Receive `headers`
   - Verify header parsing
   - Test checkpoint validation

### G.1.2: Peer Connection Lifecycle ❌

**Tests to Create:**
1. **Connection Establishment**
   - Outbound connection succeeds
   - Inbound connection accepted
   - Connection to invalid address fails
   - Connection timeout

2. **Handshake Completion**
   - Full handshake (version ↔ verack)
   - Peer added to connected_peers_ map
   - PeerInfo populated correctly
   - Handshake timeout

3. **Keepalive Behavior**
   - Ping sent periodically
   - Pong received
   - Latency tracked
   - Peer timeout (no pong)

4. **Graceful Disconnect**
   - `disconnect_peer()` works
   - Socket closed
   - Peer removed from map
   - Handler callback invoked

5. **Error Handling**
   - Connection lost (broken socket)
   - Invalid message received
   - Checksum mismatch
   - Protocol violation

### G.1.3: Network Message Serialization ❌

**Tests to Create:**
1. **Message Header**
   - Serialize/deserialize correctly
   - Magic bytes match network
   - Command field (12 bytes, null-terminated)
   - Length field correct
   - Checksum validation

2. **Payload Serialization**
   - version message (protocol version, height, etc.)
   - addr message (peer list)
   - inv message (inventory items)
   - getdata message (requested items)
   - headers message (header list)
   - block message (full block)

3. **Checksum Validation**
   - Correct checksum accepted
   - Incorrect checksum rejected
   - Empty payload (0 length)

4. **Message Size Limits**
   - Normal message accepted
   - Oversized message rejected
   - Empty message handled

---

## Test Infrastructure Needed

### Multi-Node Test Harness

**Requirements:**
- Spawn multiple P2P nodes (3-5 nodes)
- Control network topology (who connects to whom)
- Inject messages programmatically
- Monitor message flow
- Simulate network conditions (latency, packet loss)

**Components to Build:**
1. **TestNode** - Wrapper around P2PManager
   - Controllable lifecycle (start/stop)
   - Message inspection (capture sent/received messages)
   - State queries (connected peers, message counts)

2. **TestNetwork** - Multi-node coordinator
   - Spawn N test nodes
   - Create connections between nodes
   - Wait for convergence (all nodes connected)
   - Shutdown all nodes

3. **Message Inspector** - Capture and analyze messages
   - Log all sent/received messages
   - Verify message order
   - Check for duplicates
   - Measure latency

4. **Network Simulator** - Simulate adverse conditions
   - Add latency (delay messages)
   - Drop packets (simulate packet loss)
   - Disconnect peers (simulate churn)

---

## Implementation Plan

### Step 1: Create Test Harness (1-2 days)
- `tests/p2p/test_harness.h` - TestNode, TestNetwork classes
- `tests/p2p/test_harness.cpp` - Implementation
- Simple test: Spawn 2 nodes, connect, verify handshake

### Step 2: Message Protocol Tests (2-3 days)
- `tests/p2p/test_message_protocol.cpp`
- All message types (version, ping, addr, inv, headers, block)
- Serialization roundtrip tests
- Checksum validation tests

### Step 3: Connection Lifecycle Tests (1-2 days)
- `tests/p2p/test_connection_lifecycle.cpp`
- Connect, handshake, keepalive, disconnect
- Error scenarios (timeout, invalid message)

### Step 4: Integration Tests (1-2 days)
- `tests/p2p/test_p2p_integration.cpp`
- Multi-node scenarios (3-5 nodes)
- Peer discovery propagation
- Block announcement relay
- Headers-first sync

---

## Success Criteria

After G.1 completion:
- ✅ P2P handshake verified (version ↔ verack)
- ✅ Ping/pong keepalive verified
- ✅ Peer discovery verified (getaddr → addr)
- ✅ Block announcement verified (inv → getdata → block)
- ✅ Headers-first sync verified (getheaders → headers)
- ✅ Connection lifecycle verified (connect → handshake → disconnect)
- ✅ Message serialization verified (all message types)
- ✅ Multi-node integration tests pass (3-5 nodes)

---

## Risks & Challenges

**1. Multi-threading Complexity**
- P2P manager uses multiple threads
- Race conditions in tests possible
- Need proper synchronization primitives

**2. Network I/O**
- Tests involve real sockets
- Flakiness from port conflicts
- Need proper cleanup (close sockets)

**3. Timing Dependencies**
- Keepalive timing (ping intervals)
- Connection timeouts
- Message propagation delays
- Need careful use of timeouts in tests

**4. Platform Differences**
- macOS vs Linux socket behavior
- Windows compatibility (if needed)
- IPv4 vs IPv6 differences

---

## Next Steps

1. ✅ Assessment complete (this document)
2. ⏭️ Create test harness (`tests/p2p/test_harness.h/cpp`)
3. ⏭️ Write first test (2-node handshake)
4. ⏭️ Expand to full message protocol tests
5. ⏭️ Multi-node integration tests

**Status:** Ready to begin test implementation
