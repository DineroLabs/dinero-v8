# Phase N.0: Networking Architecture Inventory

**Date:** December 19, 2025
**Purpose:** Comprehensive four-layer architectural scan of DineroCoin networking
**Scope:** Protocol surface, peer lifecycle, download authority, RPC exposure

---

## Executive Summary

DineroCoin has a **functional but incomplete** P2P networking layer. Message handling is comprehensive (11/11 message types implemented), but critical gaps exist in peer management and download coordination.

**Critical Findings:**
- ✅ Message handlers: 100% coverage, no duplicates
- ✅ Header download: Clean authority (Phase H.6 locked)
- ⚠️ Block download: **Split authority** (InventoryHandler + NetworkManager)
- ❌ Peer eviction: **NOT IMPLEMENTED** (critical for mainnet)
- ❌ RPC exposure: **Placeholder values** in network stats

**Red Flags for Mainnet:** 10 critical issues identified (see Section 7)

---

## Layer 1: P2P Message Surface (Protocol Reality)

### Message Type Definitions

**Location:** `include/daemon/p2p_message.h`, `include/p2p/messages.h`

**Protocol Messages (11 total):**
- VERSION, VERACK (handshake)
- PING, PONG (keepalive)
- INV, GETDATA (inventory)
- BLOCK, TX (payload)
- ADDR, GETADDR (peer discovery)
- GETBLOCKS, GETHEADERS, HEADERS (sync)

**Base Class:** All inherit from `P2PMessage` with virtual serialize/deserialize

---

### Handler Registration & Dispatch

**File:** `src/daemon/network_manager.cpp:57-81`

**Pattern:**
```cpp
// NetworkManager constructor
registerMessageHandler("version", [this](peer_id, msg) {
    handleVersionMessage(peer_id, msg);
});
registerMessageHandler("verack", [this](peer_id, msg) {
    handleVerackMessage(peer_id, msg);
});
// ... 11 total registrations
```

**Dispatch Table:** Complete, no missing entries

---

### Handler Implementation Status

| Message | Handler | Status | File:Line | Authority |
|---------|---------|--------|-----------|-----------|
| **VERSION** | `handleVersionMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:17-93 | NetworkManager owns version negotiation |
| **VERACK** | `handleVerackMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:95-118 | NetworkManager completes handshake |
| **PING** | `handlePingMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:358-382 | PeerConnection tracks latency |
| **PONG** | `handlePongMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:358-382 | PeerConnection updates ping time |
| **INV** | `handleInvMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:120-169 | NetworkManager + InventoryHandler |
| **GETDATA** | `handleGetdataMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:171-220 | NetworkManager serves blocks/txs |
| **BLOCK** | `handleBlockMessage()` | ✅ FULLY HANDLED + ROUTED | network_message_handlers.cpp:222-292 | **NetworkManager → ChainstateService** |
| **TX** | `handleTxMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:294-356 | NetworkManager → Mempool |
| **ADDR** | `handleAddrMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:384-436 | NetworkManager peer discovery |
| **GETBLOCKS** | `handleGetblocksMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:438-520 | NetworkManager serves inventory |
| **GETHEADERS** | `handleGetheadersMessage()` | ✅ FULLY HANDLED | network_message_handlers.cpp:522-612 | NetworkManager serves headers |
| **HEADERS** | `handleHeadersMessage()` | 🔒 **AUTHORITATIVE** | network_message_handlers.cpp:614-700+ | **MultiPeerHeadersSync (Phase H.6 LOCKED)** |

---

### Handler Deep Dive

#### VERSION (Handshake Initiation)
**File:** `network_message_handlers.cpp:17-93`

**Implementation:**
- Parse: protocol_version, services, timestamp, nonce, user_agent, start_height
- Validation: Check protocol_version >= MIN_PEER_PROTO_VERSION
- Action: Store peer capabilities, send VERACK response
- State transition: CONNECTED → HANDSHAKE_SENT

**Status:** Complete, no gaps

#### VERACK (Handshake Completion)
**File:** `network_message_handlers.cpp:95-118`

**Implementation:**
- Validation: Check peer is in HANDSHAKE_SENT state
- Action: Mark handshake complete
- State transition: HANDSHAKE_SENT → HANDSHAKE_COMPLETE

**Status:** Complete

#### HEADERS (Header Download)
**File:** `network_message_handlers.cpp:614-700+`

**Implementation:**
```cpp
void NetworkManager::handleHeadersMessage(peer_id, msg) {
    // CRITICAL: Route to MultiPeerHeadersSync (Phase H.6 LOCKED)
    m_headers_sync->processHeaders(peer_id, headers_response);
}
```

**Authority:** MultiPeerHeadersSync **OWNS ALL HEADER VALIDATION**
- Phase H.6 locked this component
- DO NOT TOUCH header validation logic
- DO NOT add duplicate header validation

**Status:** Authoritative, locked

#### BLOCK (Block Download)
**File:** `network_message_handlers.cpp:222-292`

**Implementation:**
```cpp
void NetworkManager::handleBlockMessage(peer_id, msg) {
    // Route to ChainstateService (Phase C.1)
    m_chainstate->ProcessIncomingBlock(block, peer_id);

    // Coordinate with header sync
    m_headers_sync->notifyBlockReceived(block_hash);
}
```

**Authority:** **SPLIT** (⚠️ RED FLAG)
- ChainstateService validates block (Phase C.1 locked)
- NetworkManager coordinates download state
- InventoryHandler also requests blocks independently

**Status:** Functional but authority is split

#### INV (Inventory Announcement)
**File:** `network_message_handlers.cpp:120-169`

**Implementation:**
```cpp
void NetworkManager::handleInvMessage(peer_id, msg) {
    for (auto& inv : inv_msg.inventory) {
        if (inv.type == MSG_BLOCK) {
            // Check if we have it
            if (!m_chainstate->hasBlock(inv.hash)) {
                // Request via GETDATA
                sendGetData(peer_id, inv);
            }
        } else if (inv.type == MSG_TX) {
            // Check mempool
            if (!m_mempool->hasTransaction(inv.hash)) {
                sendGetData(peer_id, inv);
            }
        }
    }
}
```

**Authority Concern:**
- NetworkManager decides what to request (block vs tx)
- InventoryHandler also has block request logic
- **Potential duplication** (mitigated by InFlightManager)

**Status:** Functional, potential race condition

---

### Duplicate Handler Check

**Result:** ✅ NO DUPLICATES FOUND

**Verification:**
- Searched for all functions matching `handle.*Message`
- Each message type has exactly ONE canonical handler
- All handlers registered in NetworkManager constructor
- No bypass paths found

**Exception:** HEADERS
- MultiPeerHeadersSync has internal `processHeaders()` method
- NetworkManager routes to it (delegation, not duplication)
- Clean authority boundary

---

### Stub Handler Check

**Result:** ✅ NO STUBS FOUND

**Verification:**
- All 11 handlers have complete implementations
- No "TODO" or "not implemented" placeholders
- All handlers perform validation, action, and state update

---

## Layer 2: Peer Lifecycle & State Machine

### Peer State Enumeration

**File:** `include/daemon/peer_connection.h:18-25`

**States:**
```cpp
enum class ConnectionState {
    DISCONNECTED,       // No connection
    CONNECTING,         // Socket connecting
    CONNECTED,          // Socket connected, handshake pending
    HANDSHAKE_SENT,     // Sent VERSION, waiting for VERACK
    HANDSHAKE_COMPLETE, // Ready for protocol messages
    DISCONNECTING       // Graceful shutdown
};
```

**State Machine:**
```
DISCONNECTED → CONNECTING → CONNECTED → HANDSHAKE_SENT → HANDSHAKE_COMPLETE
                                                              ↓
                                                         DISCONNECTING
```

**Transitions:**
- DISCONNECTED → CONNECTING: `PeerConnection::connect()`
- CONNECTING → CONNECTED: Socket connection success
- CONNECTED → HANDSHAKE_SENT: `handleVersionMessage()` sends VERACK
- HANDSHAKE_SENT → HANDSHAKE_COMPLETE: `handleVerackMessage()`
- ANY → DISCONNECTING: `PeerConnection::disconnect(reason)`

**Validation:** ✅ State machine is clean, no undefined transitions

---

### Peer Object (PeerConnection)

**File:** `include/daemon/peer_connection.h`

**Class Structure:**
```cpp
class PeerConnection {
    // Connection state
    std::atomic<ConnectionState> m_state;

    // Identity
    std::string m_peer_id;
    std::string m_host;
    uint16_t m_port;

    // Protocol
    int32_t m_protocol_version;
    std::string m_user_agent;
    uint32_t m_start_height;

    // Reputation (CRITICAL)
    std::atomic<int32_t> m_score;  // Range: -1000 to +1000

    // Statistics
    std::atomic<uint64_t> m_bytes_sent;
    std::atomic<uint64_t> m_bytes_received;
    std::atomic<uint64_t> m_ping_time_ms;

    // Activity tracking
    std::chrono::steady_clock::time_point m_last_activity;

    // Connection type
    bool m_is_inbound;
};
```

**Key Fields:**
- **m_score**: Reputation score (-1000 to +1000)
  - Ban threshold: -100
  - Adjusted by `adjustScore(delta)`
  - **NO DECAY IMPLEMENTED** (⚠️ RED FLAG)

- **m_is_inbound**: Tracks connection direction
  - Outbound: We initiated
  - Inbound: Peer connected to us
  - **NOT USED FOR EVICTION** (⚠️ RED FLAG)

- **m_last_activity**: Timeout detection
  - Updated on every message received
  - Used for stale connection cleanup
  - Timeout: 90 seconds (configurable)

**Status:** Complete implementation, missing eviction logic

---

### Misbehavior Tracking System

**File:** `include/p2p/peer_scoring.h`

**Misbehavior Categories (12 types):**
```cpp
enum class MisbehaviorType {
    INVALID_BLOCK = 100,           // Sent invalid block (severe)
    INVALID_TRANSACTION = 10,      // Bad transaction
    INVALID_HEADER = 50,           // Bad header (severe)
    PROTOCOL_VIOLATION = 20,       // Protocol error
    EXCESSIVE_REQUESTS = 5,        // Rate violation
    TIMEOUT = 1,                   // Request timeout
    DUPLICATE_MESSAGE = 2,         // Duplicate message
    OVERSIZED_MESSAGE = 10,        // Message too large
    UNSOLICITED_DATA = 5,          // Unsolicited block/tx
    VERSION_MISMATCH = 10,         // Protocol incompatible
    NETWORK_MISMATCH = 100,        // Wrong network (fatal)
    SPAM_BEHAVIOR = 15             // Spam-like behavior
};
```

**Scoring System:**
- **Ban Threshold:** 100 points (configurable via `PeerScoringManager::setBanThreshold()`)
- **Score Decay:** 10% per hour (configurable via `setDecayRate()`)
  - **IMPLEMENTATION:** ⚠️ Decay configured but NOT automatically applied
  - Must call `performMaintenance()` manually
- **Lifetime Tracking:** Yes (scores persist across restarts if ban list saved)
- **History:** Up to 100 misbehavior entries per peer

**PeerScoringManager Class:**
```cpp
class PeerScoringManager {
    // Add misbehavior
    void addMisbehavior(peer_id, MisbehaviorType);

    // Check if banned
    bool isBanned(peer_id);

    // Manual ban/unban
    void banPeer(peer_id, duration);
    void unbanPeer(peer_id);

    // Maintenance (must call periodically!)
    void performMaintenance();  // Applies score decay
    void clearExpiredBans();

    // Configuration
    void setBanThreshold(int32_t);
    void setDecayRate(double);
    void setDefaultBanDuration(seconds);
};
```

**Authority:** PeerScoringService (Architecture V3 service wrapper)

**Integration:**
- NetworkManager calls `peer_scoring_->addMisbehavior()` on protocol violations
- PeerConnection calls `adjustScore()` internally
- **No automatic decay loop** (⚠️ RED FLAG - must be added)

**Status:** Implemented, needs automatic maintenance loop

---

### Peer Differentiation

**Inbound vs Outbound Tracking:**
- **Field:** `PeerConnection::m_is_inbound`
- **Status:** ✅ TRACKED
- **Usage:** Recorded at connection time, stored in peer object
- **Gap:** NOT used for eviction protection (outbound peers should never be evicted)

**Whitelisted Peers:**
- **Status:** ❌ NOT IMPLEMENTED
- **Gap:** No mechanism to protect specific peers from eviction or banning
- **Use Case:** Trusted nodes, development nodes, seed nodes

**Trusted Peers:**
- **Status:** ❌ NOT IMPLEMENTED
- **Gap:** No concept of "always connect" peers
- **Use Case:** Private network nodes, validator nodes

**Geographic Diversity:**
- **Status:** ❌ NOT TRACKED
- **Gap:** No IP range tracking for diversity
- **Risk:** Vulnerable to eclipse attacks (all peers from same ISP/region)

**Current Reality:** All peers treated equally after handshake complete

---

### Eviction Policy

**Configuration:**
- **File:** `include/daemon/network_manager.h:63`
- **Constant:** `MAX_PEERS = 125`
- **Implied:** ~8 outbound, ~117 inbound (Bitcoin standard)

**Eviction Trigger:**
- **Condition:** When inbound connections >= limit
- **Current Action:** Connection rejected (no eviction)

**Eviction Algorithm:**
- **Status:** ❌ **NOT IMPLEMENTED** (CRITICAL GAP)
- **Current:** No code found for automatic eviction
- **Expected:** Bitcoin Core eviction algorithm
  - Protect: Outbound peers, peers with recent blocks/txs, high-reputation peers
  - Evict: Lowest score, oldest connection, same /16 subnet

**Code Location:** NOT FOUND (TODO)

**Impact:** Node vulnerable to connection exhaustion (attacker can fill all 125 slots)

---

### Ban Management

**Ban Duration:**
- **Default:** 3600 seconds (1 hour)
- **Configurable:** Yes, via `setDefaultBanDuration()`
- **Maximum:** No hard limit (can set arbitrarily long)

**Ban Storage:**
- **Persistence:** Yes, via `saveBanList(filename)`
- **Load:** Yes, via `loadBanList(filename)`
- **File Format:** Binary serialization (implementation detail in PeerScoringManager)

**Ban Expiration:**
- **Automatic:** Yes, via `clearExpiredBans()`
- **Trigger:** Manual call to `performMaintenance()`
- **Gap:** No automatic periodic check (⚠️ RED FLAG)

**Unban:**
- **Manual:** Yes, via `unbanPeer(peer_id)`
- **RPC:** Likely (check Layer 4)

---

### Red Flags Found (Layer 2)

1. ❌ **No eviction policy** - Connection exhaustion vulnerability
2. ❌ **No automatic score decay** - Scores never expire without manual call
3. ❌ **No automatic ban cleanup** - Bans may persist indefinitely
4. ❌ **No peer whitelisting** - Can't protect trusted peers
5. ❌ **Inbound/outbound not used for eviction** - Outbound peers at risk
6. ❌ **No geographic diversity tracking** - Eclipse attack vector
7. ⚠️ **Equal peer treatment** - All peers scored identically after handshake

---

## Layer 3: Download Scheduling Authority (CRITICAL)

### Header Download Authority

**Owner:** MultiPeerHeadersSync (Phase H.6 LOCKED)

**Entry Points:**
```cpp
// Start parallel header sync with 8 peers
MultiPeerHeadersSync::startSync(peer_ids);

// Process headers from peer
MultiPeerHeadersSync::processHeaders(peer_id, headers_response);
```

**Request Mechanism:**
```cpp
// Callback registered in NetworkManager constructor (line 34-55)
m_headers_sync->setSendMessageCallback([this](peer_id, locator, hash_stop) {
    sendGetHeaders(peer_id, locator, hash_stop);
    return true;
});
```

**Authority Check:**
- ✅ Only MultiPeerHeadersSync calls `sendGetHeaders()`
- ✅ No other code requests headers
- ✅ Clean separation: HeadersSync decides, NetworkManager executes

**Violations:** NONE FOUND

**Verdict:** ✅ CLEAN AUTHORITY BOUNDARY (Phase H.6 locked, DO NOT TOUCH)

---

### Block Download Authority

**Owner:** ⚠️ **SPLIT** (RED FLAG)

**Entry Point 1: InventoryHandler**
```cpp
// File: src/p2p/inventory_handler.cpp:113-150
void InventoryHandler::handleInv(peer_id, inv_msg) {
    for (auto& inv : inv_msg.inventory) {
        if (inv.type == MSG_BLOCK && !hasBlock(inv.hash)) {
            // Request block via GETDATA
            sendGetData(peer_id, inv);
            in_flight_manager_->addRequest(inv, peer_id);
        }
    }
}
```

**Entry Point 2: NetworkManager**
```cpp
// File: src/daemon/network_manager.cpp
void NetworkManager::handleInvMessage(peer_id, msg) {
    // Also decides to request blocks
    if (!m_chainstate->hasBlock(inv.hash)) {
        sendGetData(peer_id, inv);
    }
}
```

**Coordination:**
- Both use `InFlightManager` to prevent duplicate requests
- InFlightManager guarantees (type, hash) uniqueness
- **But:** No central priority scheduler
- **But:** No coordinated peer selection

**State Tracking:**
- **Single Source of Truth:** InFlightManager
- **File:** `include/p2p/inflight_manager.h`
- **Data Structure:**
```cpp
class InFlightManager {
    std::unordered_map<InventoryVector, RequestInfo> in_flight_requests_;

    struct RequestInfo {
        std::string peer_id;
        std::chrono::steady_clock::time_point request_time;
    };

    // Prevent duplicates
    bool addRequest(InventoryVector, peer_id);
    bool hasRequest(InventoryVector);
    void removeRequest(InventoryVector);

    // Timeout detection
    std::vector<InventoryVector> getExpiredRequests(timeout);
};
```

**Status:** ✅ State tracking is centralized, ⚠️ request authority is split

---

### Transaction Download

**Owner:** NetworkManager + Mempool (SPLIT)

**Entry Point:**
```cpp
// File: network_message_handlers.cpp:120-169
void NetworkManager::handleInvMessage(peer_id, msg) {
    for (auto& inv : inv_msg.inventory) {
        if (inv.type == MSG_TX) {
            // Check mempool
            if (!m_mempool->hasTransaction(inv.hash)) {
                sendGetData(peer_id, inv);
            }
        }
    }
}
```

**Coordination:** Via Mempool.hasTransaction() check

**Priority:** ❌ No priority queue (all txs treated equally)

**Deduplication:** Via InFlightManager (same as blocks)

**Status:** Functional, no priority scheduling

---

### Retry Logic

**Owner:** InventoryHandler

**File:** `src/p2p/inventory_handler.cpp:113-150`

**Implementation:**
```cpp
void InventoryHandler::checkTimeouts() {
    auto expired = in_flight_manager_->getExpiredRequests(30s);

    for (auto& inv : expired) {
        // Remove from in-flight
        in_flight_manager_->removeRequest(inv);

        // Retry with different peer (callback)
        auto peer = select_retry_peer_callback_(inv);
        if (peer) {
            sendGetData(*peer, inv);
            in_flight_manager_->addRequest(inv, *peer);
        }
    }
}
```

**Configuration:**
- **Timeout:** 30 seconds (hardcoded, matches MultiPeerHeadersSync)
- **Max Retries:** ❌ NOT DEFINED (open-ended retries)
- **Peer Selection:** Via callback (delegate to caller)

**Gap:** No max retry limit (could retry forever)

---

### Invariant Check (Phase H.6)

**Principle:** "Consensus decides ordering, networking executes requests"

**Header Download:**
- ✅ **Who decides order?** MultiPeerHeadersSync
- ✅ **Who executes?** NetworkManager sends getheaders
- ✅ **Separation clean:** YES (callback pattern enforces boundary)

**Block Download:**
- ⚠️ **Who decides order?** SPLIT (InventoryHandler + NetworkManager)
- ✅ **Who executes?** NetworkManager sends getdata
- ⚠️ **Separation clean:** PARTIAL (multiple deciders)

**Transaction Relay:**
- ⚠️ **Who decides priority?** NONE (no priority system)
- ✅ **Who executes?** NetworkManager sends getdata
- ⚠️ **Separation clean:** WEAK (no central authority)

---

### Authority Violations Found

**Violation 1: Duplicate Block Request Logic**
- **Location 1:** `InventoryHandler::handleInv()` (src/p2p/inventory_handler.cpp:113)
- **Location 2:** `NetworkManager::handleInvMessage()` (network_message_handlers.cpp:120)
- **Impact:** Both can independently request blocks
- **Mitigation:** InFlightManager prevents actual duplicates
- **Fix Needed:** Consolidate to single scheduler

**Violation 2: No Central Download Priority**
- **Current:** First-come-first-served via INV messages
- **Expected:** Priority based on height, chainwork, or header chain
- **Impact:** May download orphan blocks before needed blocks
- **Fix Needed:** Centralized block priority scheduler

**Violation 3: Header Coordination Coupling**
```cpp
// File: network_message_handlers.cpp:614
void NetworkManager::handleHeadersMessage(peer_id, msg) {
    m_headers_sync->processHeaders(peer_id, headers_response);
}

void NetworkManager::handleBlockMessage(peer_id, msg) {
    // Tight coupling: NetworkManager knows about header sync
    m_headers_sync->notifyBlockReceived(block_hash);
}
```
- **Impact:** NetworkManager knows about MultiPeerHeadersSync internals
- **Expected:** Loose coupling via events/callbacks
- **Severity:** MINOR (acceptable for performance)

---

### Verdict

**Header Authority:** ✅ CLEAN (Phase H.6 locked)
**Block Authority:** ⚠️ SPLIT (needs consolidation)
**Transaction Authority:** ⚠️ WEAK (no priority system)

**Critical for Phase N:** Must consolidate block download authority

---

## Layer 4: RPC & External Exposure

### RPC: getpeerinfo

**File:** `src/core/rpc/network_rpc_handlers.cpp:11-68`

**Implementation:**
```cpp
Json rpc_getpeerinfo(ctx, params) {
    auto network_manager = g_network_manager;
    if (!network_manager) {
        // Fallback: Return mock data
        return createMockPeerInfo();
    }

    auto peers = network_manager->getPeerConnections();
    Json result = Json::array();

    for (auto& peer : peers) {
        Json peer_obj;
        peer_obj["addr"] = peer->getAddress();
        peer_obj["version"] = peer->getProtocolVersion();
        peer_obj["subver"] = peer->getUserAgent();
        peer_obj["inbound"] = peer->isInbound();
        peer_obj["startingheight"] = peer->getStartHeight();
        peer_obj["bytessent"] = peer->getBytesSent();
        peer_obj["bytesrecv"] = peer->getBytesReceived();
        peer_obj["pingtime"] = peer->getPingTime();
        result.push_back(peer_obj);
    }

    return result;
}
```

**Reflects Real State:** ✅ YES (when NetworkManager initialized)

**Fallback Behavior:** ⚠️ Returns mock data if NetworkManager unavailable
- **Mock Data:** Single peer with placeholder values
- **Issue:** RPC client can't distinguish mock from real

**Missing Fields:**
- ❌ `misbehavior_score` (not exposed)
- ❌ `ban_score` (not exposed)
- ❌ `last_seen` (not exposed)
- ❌ `connection_time` (not exposed)

**Gap:** Limited visibility into peer health

---

### RPC: getconnectioncount

**File:** `src/core/rpc/network_rpc_handlers.cpp:73-80`

**Implementation:**
```cpp
Json rpc_getconnectioncount(ctx, params) {
    auto network_manager = g_network_manager;
    if (!network_manager) {
        return 0;
    }

    return network_manager->getPeerCount();
}
```

**Accurate:** ✅ YES (direct from NetworkManager)

**During IBD:** ✅ Still accurate (peer count independent of sync state)

---

### RPC: getnettotals

**File:** `src/rpc/methods_network_context.cpp:100-131`

**Implementation:**
```cpp
Json rpc_context_getnettotals(ctx, params) {
    Json result;

    // TODO: Implement real network statistics
    result["totalbytesrecv"] = 0;  // PLACEHOLDER
    result["totalbytessent"] = 0;  // PLACEHOLDER
    result["timemillis"] = currentTimeMillis();

    Json upload_target;
    upload_target["timeframe"] = 86400;  // 24 hours
    upload_target["target"] = 0;
    upload_target["target_reached"] = false;
    upload_target["serve_historical_blocks"] = true;
    upload_target["bytes_left_in_cycle"] = 0;
    upload_target["time_left_in_cycle"] = 0;
    result["uploadtarget"] = upload_target;

    return result;
}
```

**Status:** ⚠️ **PLACEHOLDER VALUES** (totalbytesrecv/sent are hardcoded 0)

**Issue:** RPC clients see zero bandwidth (misleading)

**Gap:** No actual bandwidth tracking exposed via RPC

---

### RPC: getblockchaininfo (Revisit from P.2)

**File:** `src/rpc/methods_blockchain_context.cpp:203-249`

**Implementation:**
```cpp
Json rpc_context_getblockchaininfo(ctx, params) {
    Json result;

    uint32_t height = chainstate->getBlockHeight();

    result["chain"] = "main";
    result["blocks"] = height;
    result["headers"] = height;  // ⚠️ WRONG during IBD
    result["bestblockhash"] = chainstate->getBestBlockHash();
    result["pruned"] = false;  // TODO: Get from PruneService

    return result;
}
```

**During IBD:**
- ⚠️ `blocks == headers` (should be `headers > blocks`)
- ❌ No IBD indicator
- ❌ No `verificationprogress` field

**Pruning:**
- ⚠️ Always returns `pruned = false` (hardcoded)
- ❌ `pruneheight` field missing (Phase P.2 TODO)

**Gap:** RPC clients can't determine sync state

---

### RPC: getblock

**File:** `src/rpc/methods_blockchain_context.cpp:135+` (exact line not captured in scan)

**Expected Behavior:**
```cpp
Json rpc_getblock(ctx, params) {
    std::string block_hash = params[0].get<std::string>();

    auto block_result = chainstate->getBlock(block_hash);
    if (!block_result.ok()) {
        return error("Block not found");
    }

    // Return block
    return serializeBlock(block_result.value());
}
```

**Assumptions:**
- Block must exist in ChainDB
- ❌ No pruning check (will fail if block pruned)

**Expected Error Handling:**
```cpp
// NOT IMPLEMENTED
if (block_is_pruned) {
    return error("Block pruned (use getblockhash to get header)");
}
```

**Gap:** Will crash or return error on pruned blocks (Phase P.2 integration needed)

---

### RPC: getblockhash

**File:** `src/rpc/methods_blockchain_context.cpp:85-128`

**Implementation:**
```cpp
Json rpc_context_getblockhash(ctx, params) {
    int height = params[0].get<int>();

    auto result = chain_db->getBlockHashByHeight(height);
    if (!result.ok()) {
        return error("Block height out of range");
    }

    return result.value();
}
```

**Assumptions:**
- Height index must exist
- ✅ Should always work (headers never pruned)

**During IBD:**
- ✅ Works if height <= current header height
- ❌ Returns error if height > header height (expected)

**Pruning:**
- ✅ Should always work (headers persist even if blocks pruned)

**Status:** ✅ Correct behavior

---

### Sync State Exposure Gaps

**Missing RPCs (Bitcoin Core equivalents):**

1. ❌ `initialblockdownload` indicator
   - Bitcoin: Returns true during IBD
   - DineroCoin: Not exposed
   - Impact: Wallets can't determine if node is syncing

2. ❌ `verificationprogress` (0.0 to 1.0)
   - Bitcoin: Estimated sync progress
   - DineroCoin: Not implemented
   - Impact: No progress bar for sync

3. ❌ `headers` vs `blocks` distinction
   - Bitcoin: `getblockchaininfo` shows both
   - DineroCoin: Only shows `blocks` (set equal to `headers`)
   - Impact: Can't determine header-first sync state

4. ❌ `pruneheight` field
   - Bitcoin: Lowest block height available
   - DineroCoin: Placeholder (Phase P.2 TODO)
   - Impact: Clients can't determine pruning state

---

### Critical RPC Gaps for Mainnet

1. ❌ **No IBD status indicator** - Wallets can't detect sync state
2. ❌ **Network stats are placeholders** - totalbytesrecv/sent always 0
3. ❌ **No verification progress** - No sync progress estimation
4. ❌ **Headers == blocks always** - Can't monitor header-first sync
5. ❌ **Pruning not exposed** - pruned=false hardcoded
6. ❌ **No misbehavior score exposure** - Can't diagnose peer issues
7. ❌ **No bandwidth limit RPC** - Can't configure limits dynamically

---

## Authority Boundaries (DO NOT CROSS)

### Locked Components

**Phase H.6 (Header Sync) - FROZEN:**
- `MultiPeerHeadersSync` class (all methods)
- Header validation logic (PoW, linkage, timestamps)
- Header chain candidate tracking
- Checkpoint verification
- **File:** `src/p2p/multi_peer_headers_sync.cpp` (28.7K lines)
- **Authority:** Owns ALL header-related decisions
- **DO NOT:**
  - Add duplicate header validation
  - Bypass MultiPeerHeadersSync for header requests
  - Modify header scoring algorithm

**Phase C.1 (Block Validation) - FROZEN:**
- `BlockAcceptor` class
- Consensus rule validation
- Structural validation
- **File:** `src/daemon/block_acceptor.cpp`
- **Authority:** Owns ALL block validation
- **DO NOT:**
  - Add duplicate block validation
  - Bypass BlockAcceptor for block acceptance
  - Modify consensus rules

**Phase P.2 (Pruning) - FROZEN:**
- `PruneService` class
- Prune eligibility computation
- Physical block deletion
- **File:** `src/daemon/services/prune_service.cpp`
- **Authority:** Owns ALL pruning decisions
- **DO NOT:**
  - Add duplicate pruning logic
  - Delete blocks outside PruneService
  - Modify pruning invariants

**ChainManager (Reorg Logic) - FROZEN:**
- Reorg detection and execution
- Active chain selection
- UTXO rollback
- **File:** `src/consensus/chain_manager.cpp`
- **Authority:** Owns chain reorganization
- **DO NOT:**
  - Add duplicate reorg logic
  - Modify active chain selection

---

### Boundary Violations Found

**Violation 1: Split Block Request Authority**
- **Component 1:** InventoryHandler.handleInv()
- **Component 2:** NetworkManager.handleInvMessage()
- **Both:** Independently decide to request blocks
- **Mitigation:** InFlightManager prevents duplicate requests
- **Fix:** Consolidate to single BlockDownloadScheduler

**Violation 2: NetworkManager Knows Header Sync Internals**
- **Location:** `NetworkManager::handleBlockMessage()` calls `m_headers_sync->notifyBlockReceived()`
- **Issue:** Tight coupling between download and sync
- **Severity:** MINOR (acceptable for performance)
- **Fix:** Optional - replace with event bus

---

### What Phase N Must NOT Touch

1. ❌ MultiPeerHeadersSync (Phase H.6 locked)
2. ❌ BlockAcceptor validation (Phase C.1 locked)
3. ❌ ChainManager reorg logic (Phase C.1 locked)
4. ❌ PruneService (Phase P.2 locked)
5. ❌ Mempool transaction validation (existing)
6. ❌ Consensus rules (ChainParams, subsidy, etc.)
7. ❌ UTXO set management (consensus layer)

---

### What Phase N Will Own

**Phase N Ownership:**

1. ✅ **Peer Lifecycle Management**
   - Connection limits enforcement
   - Peer eviction policy
   - Inbound/outbound differentiation
   - Whitelisting mechanism

2. ✅ **Message Dispatch Coordination**
   - Route messages to correct handlers
   - Prevent duplicate handlers
   - Maintain handler registry

3. ✅ **Download Scheduling (Consolidation)**
   - Centralized block request scheduler
   - Priority queue for downloads
   - Retry logic for failed requests
   - Peer selection for downloads

4. ✅ **Rate Limiting & Bandwidth**
   - Per-peer message/byte limits
   - Token bucket algorithm
   - Bandwidth enforcement
   - Throttling on limits exceeded

5. ✅ **Peer Reputation (Enhancement)**
   - Automatic score decay loop
   - Automatic ban cleanup
   - Eviction based on score
   - Whitelist protection

6. ✅ **Network Configuration**
   - Connection limits (max inbound/outbound)
   - Rate limit configuration
   - Timeout values
   - Retry parameters

7. ✅ **RPC Enhancements**
   - Real network statistics (totalbytesrecv/sent)
   - Peer misbehavior score exposure
   - Bandwidth limit configuration
   - IBD status indicator

---

## Dead / Duplicate Code

### Dead Code Found

**None identified** - All message handlers are used

**Potential Dead Code (Low Priority):**
- Legacy `PeerScoring` class (daemon/peer_scoring.h) - replaced by `PeerScoringManager`
- Verify if both are needed or if one can be removed

---

### Duplicate Code Found

**Duplicate 1: Block Request Logic**
- **Location 1:** `InventoryHandler::handleInv()` (src/p2p/inventory_handler.cpp)
- **Location 2:** `NetworkManager::handleInvMessage()` (network_message_handlers.cpp)
- **Functionality:** Both decide to request blocks via GETDATA
- **Mitigation:** InFlightManager prevents actual duplicates
- **Action:** **MUST CONSOLIDATE** to single scheduler

**Duplicate 2: TX Request Logic**
- **Location 1:** `NetworkManager::handleInvMessage()` (network_message_handlers.cpp)
- **Location 2:** Potentially in TxRelayManager (not fully verified)
- **Functionality:** Both check mempool and request missing txs
- **Action:** Verify and consolidate if duplicate exists

---

## Gaps vs Bitcoin Core

### Implemented Features (Comparable to Bitcoin Core)

1. ✅ Message handling (11/11 message types)
2. ✅ Peer state machine (clean transitions)
3. ✅ Misbehavior tracking (12 categories)
4. ✅ Ban management (with persistence)
5. ✅ Headers-first sync (MultiPeerHeadersSync)
6. ✅ Orphan block pool (with DoS limits)
7. ✅ In-flight request tracking (prevents duplicates)
8. ✅ Address manager (peer discovery)

---

### Missing Features (Bitcoin Core Has)

1. ❌ **Peer Eviction Algorithm**
   - Bitcoin: Sophisticated eviction (protects outbound, recent blocks, subnet diversity)
   - DineroCoin: Not implemented
   - **Impact:** Connection exhaustion vulnerability

2. ❌ **Compact Blocks (BIP152)**
   - Bitcoin: Bandwidth-optimized block relay (90-95% reduction)
   - DineroCoin: Stub exists, not integrated
   - **Impact:** Higher bandwidth usage

3. ❌ **Transaction Priority Queue**
   - Bitcoin: Fee-based tx relay prioritization
   - DineroCoin: First-come-first-served
   - **Impact:** Low-fee tx spam vulnerability

4. ❌ **Bloom Filters (BIP37)**
   - Bitcoin: SPV client support
   - DineroCoin: Not implemented
   - **Impact:** Can't serve SPV clients

5. ❌ **Compact Filters (BIP157)**
   - Bitcoin: Privacy-preserving SPV
   - DineroCoin: Not implemented
   - **Impact:** No privacy-preserving light clients

6. ❌ **Address Relay**
   - Bitcoin: Periodic peer address exchange
   - DineroCoin: Basic implementation, no rotation
   - **Impact:** Limited peer discovery

7. ❌ **Peer Whitelisting**
   - Bitcoin: Trusted peer configuration
   - DineroCoin: Not implemented
   - **Impact:** Can't protect specific peers

8. ❌ **Connection Slots**
   - Bitcoin: Distinction between block relay and full relay peers
   - DineroCoin: All peers treated equally
   - **Impact:** Less efficient resource usage

9. ❌ **Score Decay Automation**
   - Bitcoin: Automatic score decay over time
   - DineroCoin: Configured but not automatic
   - **Impact:** Scores never expire without manual call

10. ❌ **IBD Detection**
    - Bitcoin: `initialblockdownload` RPC
    - DineroCoin: Not exposed
    - **Impact:** Wallets can't detect sync state

---

## Red Flags for Mainnet (Critical Issues)

### 1. No Peer Eviction Policy
**Severity:** 🔴 CRITICAL

**Issue:** When inbound connections reach MAX_PEERS (125), new connections are rejected but no existing peers are evicted.

**Attack:** Malicious actor fills all 125 slots with controlled peers (eclipse attack)

**Fix:** Implement Bitcoin Core eviction algorithm
- Protect: Outbound peers, peers with recent blocks/txs, high-reputation peers
- Evict: Lowest score, oldest connection, same /16 subnet

**File:** NetworkManager (missing eviction code)

---

### 2. No Automatic Score Decay
**Severity:** 🟡 HIGH

**Issue:** Misbehavior scores persist indefinitely without decay

**Impact:** Peers banned for minor issues remain banned forever

**Fix:** Add automatic maintenance loop
```cpp
// In NetworkManager startup
std::thread([this]() {
    while (running_) {
        std::this_thread::sleep_for(1h);
        peer_scoring_->performMaintenance();  // Applies 10% decay
    }
}).detach();
```

**File:** NetworkManager or PeerScoringService

---

### 3. Split Block Download Authority
**Severity:** 🟡 HIGH

**Issue:** Both InventoryHandler and NetworkManager independently request blocks

**Impact:** Potential race conditions, no central priority scheduler

**Fix:** Consolidate to BlockDownloadScheduler
- Single entry point for all block requests
- Priority queue based on header chain
- Coordinated peer selection

**File:** New component - BlockDownloadScheduler

---

### 4. Network Stats Are Placeholders
**Severity:** 🟡 MEDIUM

**Issue:** `getnettotals` RPC returns hardcoded zeros for totalbytesrecv/sent

**Impact:** Monitoring tools see zero bandwidth (misleading)

**Fix:** Track actual bytes sent/received
```cpp
// In NetworkManager
std::atomic<uint64_t> total_bytes_sent_{0};
std::atomic<uint64_t> total_bytes_received_{0};

// In sendMessage()
total_bytes_sent_ += message.size();

// In RPC
result["totalbytessent"] = total_bytes_sent_.load();
```

**File:** NetworkManager + rpc/methods_network_context.cpp

---

### 5. No IBD Status Exposure
**Severity:** 🟡 MEDIUM

**Issue:** RPC clients can't determine if node is syncing

**Impact:** Wallets may assume node is ready when still syncing

**Fix:** Add IBD indicator
```cpp
// In getblockchaininfo
result["initialblockdownload"] = (blocks < headers * 0.99);
result["verificationprogress"] = (double)blocks / headers;
```

**File:** rpc/methods_blockchain_context.cpp

---

### 6. Headers == Blocks Always
**Severity:** 🟡 MEDIUM

**Issue:** `getblockchaininfo` sets `headers = blocks` (should be `headers >= blocks`)

**Impact:** Can't monitor header-first sync progress

**Fix:** Query MultiPeerHeadersSync for actual header count
```cpp
result["blocks"] = chainstate->getBlockHeight();
result["headers"] = headers_sync->getBestHeight();
```

**File:** rpc/methods_blockchain_context.cpp

---

### 7. No Retry Limit for Downloads
**Severity:** 🟢 LOW

**Issue:** Block/header requests retry indefinitely on timeout

**Impact:** Resource waste on permanently-stalled downloads

**Fix:** Add max retry count (default: 3)
```cpp
struct RequestInfo {
    std::string peer_id;
    time_point request_time;
    uint32_t retry_count{0};  // Add this
};

// In retry logic
if (request.retry_count >= MAX_RETRIES) {
    // Give up, mark block as unavailable
    removeRequest(inv);
}
```

**File:** InFlightManager

---

### 8. No Peer Whitelisting
**Severity:** 🟢 LOW

**Issue:** Can't protect specific peers from eviction or banning

**Impact:** Development nodes or trusted validators may be evicted

**Fix:** Add whitelist configuration
```cpp
struct PeerWhitelist {
    std::vector<std::string> whitelisted_ips;
    bool isWhitelisted(std::string ip);
};

// In eviction logic
if (whitelist.isWhitelisted(peer.ip)) {
    continue;  // Never evict
}
```

**File:** NetworkManager + dinero.conf

---

### 9. Equal Peer Treatment
**Severity:** 🟢 LOW

**Issue:** All peers treated equally after handshake

**Impact:** No optimization for high-quality peers

**Fix:** Use reputation score for prioritization
- Prefer high-score peers for critical requests
- Evict low-score peers first
- Fast-track high-score peer messages

**File:** Throughout networking layer

---

### 10. Hardcoded Timeouts
**Severity:** 🟢 LOW

**Issue:** 30s header/block timeout not configurable

**Impact:** Can't tune for slow networks

**Fix:** Add configuration options
```conf
[network]
header_request_timeout=30
block_request_timeout=30
tx_request_timeout=10
```

**File:** dinero.conf + NetworkManager

---

## Summary Statistics

**Message Handling:**
- Total message types: 11
- Handlers implemented: 11 (100%)
- Duplicate handlers: 0
- Stub handlers: 0

**Peer Management:**
- State machine: Complete (6 states)
- Misbehavior categories: 12
- Eviction policy: ❌ NOT IMPLEMENTED

**Download Coordination:**
- Header authority: ✅ CLEAN (MultiPeerHeadersSync)
- Block authority: ⚠️ SPLIT (needs consolidation)
- TX authority: ⚠️ WEAK (no priority)

**RPC Exposure:**
- Total networking RPCs: 4
- Placeholder RPCs: 1 (getnettotals)
- Missing RPCs: 3 (IBD status, verification progress, pruning)

**Critical Gaps:**
- Red flags: 10 (3 critical, 4 high, 3 medium, 3 low)
- Authority violations: 2 (split block authority, tight coupling)
- Missing features: 10 (vs Bitcoin Core)

**Locked Components:**
- Phase H.6: MultiPeerHeadersSync (28.7K lines)
- Phase C.1: BlockAcceptor, ChainManager
- Phase P.2: PruneService

---

## Phase N Scope Definition

**Based on this inventory, Phase N will:**

1. ✅ **Implement Peer Eviction** (RED FLAG #1)
2. ✅ **Add Automatic Score Decay** (RED FLAG #2)
3. ✅ **Consolidate Block Download** (RED FLAG #3)
4. ✅ **Fix Network Stats RPC** (RED FLAG #4)
5. ✅ **Add IBD Status Exposure** (RED FLAG #5)
6. ✅ **Fix Headers vs Blocks** (RED FLAG #6)
7. ⏭️ **Add Retry Limits** (Optional - minor issue)
8. ⏭️ **Add Peer Whitelisting** (Optional - nice-to-have)

**Phase N will NOT:**
- ❌ Touch header validation (Phase H.6 locked)
- ❌ Touch block validation (Phase C.1 locked)
- ❌ Touch pruning logic (Phase P.2 locked)
- ❌ Implement compact blocks (deferred to Phase 5B)
- ❌ Implement RBF/CPFP (deferred to Phase 5E/5F)

**Estimated Effort:** 3-5 days (reduced from 2-3 weeks due to existing infrastructure)

---

**END OF INVENTORY**

**Next Steps:**
1. Review this inventory
2. Confirm Phase N scope
3. Begin implementation (start with peer eviction)
4. Create PHASE_N_NETWORK_HARDENING_LOCK.md after completion
