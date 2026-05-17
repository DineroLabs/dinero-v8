# CRITICAL ARCHITECTURAL ISSUE: Dual P2P Networking Stacks

> Historical note as of 2026-03-09:
> This issue is resolved. Active daemon networking is now `P2PService -> P2PManager`.
> The legacy `NetworkManager`/`network_message_handlers` path has been removed from
> the main build and source tree. Keep the rest of this document as incident history,
> not current architecture.

**Date:** 2025-12-07
**Severity:** HIGH - Blocks continuous multi-node synchronization
**Status:** RESOLVED (historical analysis retained below)

---

## Executive Summary

DineroCoin has **TWO SEPARATE P2P networking stacks** running in parallel:

1. **P2PManager** (src/daemon/p2p_manager.cpp) - Legacy system
2. **NetworkManager + PeerConnection** (src/daemon/network_manager.cpp + peer_connection.cpp) - Modern system

These systems **do not coordinate** and compete for socket ownership, causing:
- ❌ Connections drop after ~1 block transfer
- ❌ Peers don't stay connected
- ❌ Multi-block propagation fails
- ❌ `getpeerinfo` shows 0 peers despite successful connections
- ❌ Random disconnections during message handling

---

## Evidence

### Symptom 1: Different Port Numbers in Logs

**Node 1 (P2PManager):**
```
Connection lost with 127.0.0.1:55107
Connection lost with 127.0.0.1:57103
```

**Node 1 (NetworkManager):**
```
[P2P] Sent block 05a0785836a9847f... to 127.0.0.1:55107
[P2P] Sent block 58e9f588d716e1a9... to 127.0.0.1:57103
```

**Node 2 (P2PManager):**
```
Connection lost with 127.0.0.1:26001 (twice)
```

**Analysis:**
- Different ephemeral ports (55107, 57103) indicate NEW connections each time
- Not persistent connections being reused
- Both systems logging disconnections from same peers

### Symptom 2: Blocks Successfully Sent But Connections Die

```
1. INV broadcast ✅
2. GETDATA received ✅
3. BLOCK sent ✅
4. Connection drops ❌
5. Next INV broadcast ❌ (no peers connected)
```

**What's Happening:**
1. P2PManager's `addnode` establishes initial connection
2. NetworkManager's message handler successfully sends BLOCK
3. P2PManager's `peer_handler_loop` closes socket (recv() returns 0)
4. PeerConnection's `networkIOThread()` also closes socket
5. Both systems think peer is disconnected
6. No persistent connection remains

### Symptom 3: Two Separate GETDATA Handlers

**Location 1 (P2PManager path):** `src/daemon/daemon_app.cpp:527`
```cpp
p2p_service->get().send_to_peer(peer_addr, block_msg);
std::cout << "[P2P] Sent block " << ... << " to " << peer_addr << std::endl;
```

**Location 2 (NetworkManager path):** `src/daemon/network_message_handlers.cpp:191`
```cpp
peer->sendMessage(block_msg);
g_logger.info("Sent block " + inv.hash + " to peer " + peer->getPeerId());
```

---

## Architecture Comparison

### P2PManager (Legacy)

**File:** `src/daemon/p2p_manager.cpp`

**Characteristics:**
- Blocking `recv()` in `peer_handler_loop()`
- Own socket lifecycle management
- Registered via `addnode` RPC
- Used by `P2PService` wrapper
- Logs: "Connection lost with..."

**Thread Model:**
```
connect_to_peer()
  ↓
peer_handler_loop() [thread]
  ↓
while (!shutdown_requested_ && peer->is_connected):
    auto message = receive_message(socket_fd)  // BLOCKING recv()
    if (!message):
        break  // Disconnects
```

**Problem:** Once `receive_message()` returns nullptr (connection closed), the loop exits and socket is cleaned up.

### NetworkManager + PeerConnection (Modern)

**Files:** `src/daemon/network_manager.cpp`, `src/daemon/peer_connection.cpp`

**Characteristics:**
- Non-blocking I/O with `networkIOThread()`
- Asynchronous message queues
- Proper message framing
- Production-ready architecture
- Logs: "Peer XX closed connection (EOF)"

**Thread Model:**
```
PeerConnection::connect()
  ↓
startIOThread()
  ↓
networkIOThread() [thread]
  ↓
while (m_running && isConnected()):
    // Send queued messages
    // Read incoming data (non-blocking)
    // Process messages
    sleep_for(10ms)  // Prevents busy-waiting
```

**Advantage:** Continuous I/O loop, queued sends, proper async handling.

---

## Why This Causes "One-Block-and-Drop"

### Current Flow (BROKEN)

```
1. User calls addnode RPC
   ↓
2. P2PService::ConnectToPeer(host, port)
   ↓
3. P2PManager::connect_to_peer() creates socket
   ↓
4. P2PManager::peer_handler_loop() starts (Thread A)
   ↓
5. Version/verack handshake completes
   ↓
6. Block mined, INV broadcast via P2PManager
   ↓
7. GETDATA arrives
   ↓
8. NetworkManager::handleGetdataMessage() sends BLOCK
   ↓
   [BUG: PeerConnection and P2PManager both own same socket]
   ↓
9. P2PManager's recv() gets EOF → closes socket
   ↓
10. PeerConnection's recv() fails → closes socket
   ↓
11. Both threads exit, peer disconnected
```

### Result

- First block: Works (ephemeral connection succeeds)
- Subsequent blocks: Fail (connection already dead)

---

## Recommended Fix

### Option 1: Quick Fix (Recommended for Immediate Deployment)

**Disable P2PManager connection handling, keep only NetworkManager:**

1. Modify `addnode` RPC to call NetworkManager directly
2. Don't invoke `P2PManager::connect_to_peer()`
3. Keep P2PManager for other functionality (message broadcasting, peer database)
4. All socket management → PeerConnection only

**Implementation:**
```cpp
// methods_network_context.cpp
din::Json rpc_context_addnode(const ExecutionContext& ctx, const din::Json& params) {
    // Parse address:port...

    // OLD (broken): Creates P2PManager connection
    // auto p2p = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
    // bool success = p2p->ConnectToPeer(host, port);

    // NEW (fixed): Create NetworkManager connection
    // TODO: Add NetworkManager to DaemonContext
    // network_manager->connectToPeer(host, port);

    return din::null();
}
```

**Pros:**
- Minimal code changes
- Keeps existing infrastructure
- Immediate fix for connection persistence

**Cons:**
- Architectural debt remains
- Two systems still exist

### Option 2: Full Refactor (Recommended for Long-Term)

**Completely deprecate P2PManager:**

1. Remove `src/daemon/p2p_manager.cpp`
2. Remove `P2PService` wrapper
3. Promote NetworkManager to IService in DaemonContext
4. Move peer database to NetworkManager
5. All P2P functionality → NetworkManager

**This matches Bitcoin Core's architecture.**

**Pros:**
- Clean, maintainable architecture
- No socket ownership conflicts
- Scalable and testable

**Cons:**
- Larger refactor (~2-3 days)
- Requires extensive testing

---

## Impact Analysis

### What Works Now

✅ P2P message protocol (INV, GETDATA, BLOCK)
✅ Block serialization/deserialization
✅ Message handlers
✅ Version/verack handshake
✅ First block propagation

### What's Broken

❌ Persistent connections
❌ Multi-block continuous sync
❌ Peer connection management
❌ `getpeerinfo` accuracy
❌ Long-running multi-node networks

### Production Readiness

**Single-Node:** ✅ READY
**Multi-Node:** ❌ BLOCKED by this issue

---

## Conclusion

The **dual P2P stack architecture** is the root cause of connection instability.

**Immediate action required:**
- Implement Option 1 (quick fix) for testnet deployment
- Plan Option 2 (full refactor) for mainnet release

**This is a showstopper for multi-node synchronization.**

---

**Analysis Date:** 2025-12-07
**Analyst:** Claude Code (Anthropic)
**Priority:** P0 - Critical
**Blocking:** Phase 29 completion, multi-node testnet
