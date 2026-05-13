# Phase N.3: Production P2P Integration - REVISED IMPLEMENTATION PLAN

**Date**: 2025-12-21
**Status**: Ready to implement
**Complexity**: LOW (simpler than initially planned)

---

## Key Discovery

**NetworkManager already uses `HeaderSyncManager`** (from Phase H)!

**Evidence**:
- `network_manager.h:264` - `HeaderSyncManager* header_sync_manager_{nullptr}`
- `network_message_handlers.cpp:660-701` - Routes headers to `header_sync_manager_` if present
- Manual 80-byte header parsing already implemented

**What This Means**:
- ✅ Header parsing: **DONE** (80-byte Bitcoin wire format → dinero::BlockHeader)
- ✅ HeaderSyncManager integration: **PARTIAL** (receives headers, but no callbacks set up)
- ⏳ HeaderSyncP2P wrapper: **NEEDED** (to add P2P wiring + callbacks)

---

## Simplified Architecture

### Current Flow (Phase H)

```
NetworkManager::handleHeadersMessage()
  ↓
Parse 80-byte headers manually
  ↓
Convert to dinero::BlockHeader (with empty Utreexo)
  ↓
header_sync_manager_->ProcessHeaders() [if set]
```

**Missing**: Callbacks for sending getheaders, disconnecting peers, peer lifecycle

### Target Flow (Phase N.3)

```
NetworkManager initialization
  ↓
Create HeaderSyncP2P(chain_selector, header_store)
  ↓
Set callbacks (SendGetheaders, DisconnectPeer, etc.)
  ↓
NetworkManager::handleHeadersMessage()
  ↓
Parse 80-byte headers (existing code)
  ↓
header_sync_p2p_->OnHeadersMessage(peer_id, headers_msg)
  ↓
HeaderSyncP2P calls callbacks as needed
```

---

## Implementation Steps (Revised)

### Step 1: Add HeaderSyncP2P to NetworkManager

**File**: `include/daemon/network_manager.h`

**Add include**:
```cpp
#include "consensus/header_sync_p2p.h"
```

**Add member** (replace both existing header sync systems):
```cpp
// Before:
std::unique_ptr<p2p::MultiPeerHeadersSync> m_headers_sync;
class HeaderSyncManager* header_sync_manager_{nullptr};

// After:
std::unique_ptr<consensus::HeaderSyncP2P> m_header_sync_p2p;
```

**Complexity**: Trivial

---

### Step 2: Initialize HeaderSyncP2P in Constructor

**File**: `src/daemon/network_manager.cpp`

**Replace** (lines 34-45):
```cpp
// Before:
m_headers_sync = std::make_unique<p2p::MultiPeerHeadersSync>();
m_headers_sync->setSendMessageCallback([...]);

// After:
// Note: HeaderChainSelector and HeaderStore need to be passed in or created
// For now, we'll need to add these as dependencies

m_header_sync_p2p = std::make_unique<consensus::HeaderSyncP2P>(
    nullptr,  // TODO: Pass HeaderChainSelector* from blockchain
    nullptr   // TODO: Pass HeaderStore* (optional)
);

// Set callbacks
setupHeaderSyncCallbacks();
```

**Add helper method**:
```cpp
void NetworkManager::setupHeaderSyncCallbacks() {
    if (!m_header_sync_p2p) return;

    // Callback: Send getheaders message
    m_header_sync_p2p->SetSendGetheadersCallback([this](
        uint64_t peer_id_uint,
        const std::vector<uint256>& locator,
        const uint256& hash_stop
    ) {
        // Convert uint64_t peer ID to string
        std::string peer_id_str = peerIdToString(peer_id_uint);

        // Find peer
        auto it = m_peers.find(peer_id_str);
        if (it == m_peers.end()) return;

        auto peer = it->second;

        // Create getheaders message
        GetheadersMessage msg;
        msg.version = 70001;

        // Convert locator
        for (const auto& hash : locator) {
            msg.block_locator_hashes.push_back(hash.GetHex());
        }

        msg.hash_stop = hash_stop.GetHex();

        // Send message
        peer->sendMessage(msg);

        g_logger.info("Sent getheaders to peer " + peer_id_str);
    });

    // Callback: Disconnect peer
    m_header_sync_p2p->SetDisconnectPeerCallback([this](
        uint64_t peer_id_uint,
        consensus::PeerSwitchReason reason
    ) {
        std::string peer_id_str = peerIdToString(peer_id_uint);

        g_logger.info("Disconnecting peer " + peer_id_str +
                      " (reason=" + std::to_string(static_cast<int>(reason)) + ")");

        // Disconnect peer
        auto it = m_peers.find(peer_id_str);
        if (it != m_peers.end()) {
            it->second->disconnect();
            m_peers.erase(it);
        }
    });

    // Callback: Send headers (for responding to getheaders requests)
    m_header_sync_p2p->SetSendHeadersCallback([this](
        uint64_t peer_id_uint,
        const std::vector<BlockHeader>& headers
    ) {
        std::string peer_id_str = peerIdToString(peer_id_uint);

        auto it = m_peers.find(peer_id_str);
        if (it == m_peers.end()) return;

        auto peer = it->second;

        // Create headers message
        HeadersMessage msg;
        for (const auto& header : headers) {
            // Serialize header to 80 bytes
            std::vector<uint8_t> header_bytes = serializeHeader(header);
            msg.headers.push_back(header_bytes);
        }

        peer->sendMessage(msg);
    });
}
```

**Add helper functions**:
```cpp
// Convert uint64_t peer ID to string (simple hash-based conversion)
std::string NetworkManager::peerIdToString(uint64_t peer_id) {
    // Reverse lookup in peer_id_map_
    auto it = peer_id_to_string_map_.find(peer_id);
    if (it != peer_id_to_string_map_.end()) {
        return it->second;
    }
    return "";
}

// Convert string peer ID to uint64_t
uint64_t NetworkManager::stringToPeerId(const std::string& peer_id_str) {
    // Check if we already have a mapping
    auto it = peer_string_to_id_map_.find(peer_id_str);
    if (it != peer_string_to_id_map_.end()) {
        return it->second;
    }

    // Create new mapping
    uint64_t peer_id = std::hash<std::string>{}(peer_id_str);
    peer_string_to_id_map_[peer_id_str] = peer_id;
    peer_id_to_string_map_[peer_id] = peer_id_str;
    return peer_id;
}
```

**Add member variables for peer ID mapping**:
```cpp
std::unordered_map<std::string, uint64_t> peer_string_to_id_map_;
std::unordered_map<uint64_t, std::string> peer_id_to_string_map_;
```

**Complexity**: Medium (callback setup + peer ID conversion)

---

### Step 3: Update handleHeadersMessage

**File**: `src/daemon/network_message_handlers.cpp`

**Simplify** (lines 603-710):
```cpp
bool NetworkManager::handleHeadersMessage(std::shared_ptr<PeerConnection> peer,
                                          const P2PMessage& message) {
    const auto& headers_msg = static_cast<const HeadersMessage&>(message);

    g_logger.info("Received " + std::to_string(headers_msg.headers.size()) +
                  " headers from peer " + peer->getPeerId());

    if (!m_header_sync_p2p) {
        g_logger.warning("HeaderSyncP2P not initialized");
        return false;
    }

    // Convert peer ID to uint64_t
    uint64_t peer_id = stringToPeerId(peer->getPeerId());

    // Delegate to HeaderSyncP2P (it will parse and validate internally)
    bool accepted = m_header_sync_p2p->OnHeadersMessage(peer_id, headers_msg);

    if (!accepted) {
        // Headers were invalid
        peer->adjustScore(-10);
        g_logger.warning("Invalid headers from peer " + peer->getPeerId());
    }

    return accepted;
}
```

**Wait**: HeaderSyncP2P needs to parse headers internally. Let me update the ParseHeadersMessage implementation.

**Actually**: Keep the existing parsing code and pass parsed headers directly!

**Better approach**:
```cpp
bool NetworkManager::handleHeadersMessage(...) {
    // ... existing parsing code (lines 614-698) ...

    // Use HeaderSyncP2P instead of header_sync_manager_
    if (m_header_sync_p2p) {
        uint64_t peer_id = stringToPeerId(peer->getPeerId());

        // NOTE: We bypass OnHeadersMessage() and call ProcessHeaders directly
        // because we've already parsed the headers above
        bool success = m_header_sync_p2p->GetSyncManager()->ProcessHeaders(
            peer_id, dinero_headers
        );

        if (!success) {
            peer->adjustScore(-10);
            return false;
        }

        return true;
    }

    // Fallback to old MultiPeerHeadersSync (if HeaderSyncP2P not available)
    if (m_headers_sync) {
        // ... existing MultiPeerHeadersSync code ...
    }
}
```

**Complexity**: Low (mostly just changing which object we call)

---

### Step 4: Wire Peer Lifecycle Events

**File**: `src/daemon/network_manager.cpp`

**In handleVersionMessage** (after handshake complete):
```cpp
// After peer state = HANDSHAKE_COMPLETE
if (m_header_sync_p2p) {
    uint64_t peer_id = stringToPeerId(peer->getPeerId());

    uint256 best_hash;
    best_hash.SetHexString(peer->getStartHash());

    m_header_sync_p2p->OnPeerConnected(
        peer_id,
        peer->getStartHeight(),
        best_hash,
        peer->isOutbound()
    );
}
```

**In cleanupStaleConnections** (when peer disconnects):
```cpp
void NetworkManager::cleanupStaleConnections() {
    // ... existing cleanup logic ...

    for (auto it = m_peers.begin(); it != m_peers.end();) {
        if (should_disconnect) {
            // Notify header sync
            if (m_header_sync_p2p) {
                uint64_t peer_id = stringToPeerId(it->first);
                m_header_sync_p2p->OnPeerDisconnected(peer_id);
            }

            it = m_peers.erase(it);
        } else {
            ++it;
        }
    }
}
```

**Complexity**: Low (simple hooks)

---

### Step 5: Start Header Sync

**File**: `src/daemon/network_manager.cpp`

**In peerMaintenanceThread** (or explicit startup):
```cpp
// Check if we should start header sync
if (m_header_sync_p2p && !m_header_sync_started) {
    auto stats = m_header_sync_p2p->GetStats();

    if (stats.headers_behind > 0 && m_peers.size() > 0) {
        g_logger.info("Starting header sync (" +
                      std::to_string(stats.headers_behind) + " headers behind)");

        m_header_sync_p2p->StartSync();
        m_header_sync_started = true;
    }
}

// Drive state machine
if (m_header_sync_p2p) {
    m_header_sync_p2p->Tick();  // Uses system time
}
```

**Complexity**: Trivial

---

### Step 6: Implement Missing TODOs in HeaderSyncP2P

**Already covered in main PHASE_N.3_PLAN.md** - ParseHeadersMessage() and FindHeadersToSend()

Actually, since we're keeping the existing parsing in NetworkManager, we can **skip ParseHeadersMessage()** and just expose GetSyncManager() for direct access.

**Add to HeaderSyncP2P.h**:
```cpp
// For cases where headers are pre-parsed by P2P layer
HeaderSyncManager* GetSyncManager() { return sync_manager_.get(); }
```

**Complexity**: Trivial

---

## Dependencies Resolution

### Required Components

1. ✅ **HeaderChainSelector** - exists, need to pass to HeaderSyncP2P constructor
2. ✅ **HeaderStore** - exists (optional)
3. ✅ **80-byte header parsing** - exists in network_message_handlers.cpp
4. ⏳ **serializeHeader()** - need to implement for SendHeaders callback
5. ⏳ **Peer ID conversion** - need bidirectional map

### Dependency Injection

**Problem**: NetworkManager constructor doesn't have access to HeaderChainSelector

**Solution**: Add setter or late initialization
```cpp
void NetworkManager::initializeHeaderSync(
    consensus::HeaderChainSelector* chain_selector,
    consensus::HeaderStore* header_store
) {
    m_header_sync_p2p = std::make_unique<consensus::HeaderSyncP2P>(
        chain_selector,
        header_store
    );

    setupHeaderSyncCallbacks();
}
```

**Called from**: DaemonApp or wherever blockchain is initialized

---

## Testing Strategy

### Phase 1: Unit Tests (Already Passing)
- ✅ 18 Phase N.2 tests lock HeaderSyncP2P behavior

### Phase 2: Integration Test
**Create**: `tests/daemon/test_network_manager_header_sync.cpp`

**Tests**:
1. Peer connects → OnPeerConnected called
2. Headers message → ProcessHeaders called
3. Stall timeout → DisconnectPeer callback invoked
4. Getheaders request → SendGetheaders callback invoked

### Phase 3: Manual Testing
1. Start dinerod with empty chain
2. Connect to test peers
3. Verify header sync completes
4. Check logs for timeout behavior

---

## Timeline

**Estimated**: 1-2 days (significantly less than original 3-day estimate)

**Breakdown**:
- Step 1-2 (Add HeaderSyncP2P + callbacks): 3 hours
- Step 3 (Update handlers): 2 hours
- Step 4-5 (Lifecycle + startup): 2 hours
- Step 6 (Implement TODOs): 1 hour
- Dependency injection setup: 2 hours
- Testing: 4 hours
- **Total**: ~14 hours = 2 days

---

## Success Criteria

✅ NetworkManager uses HeaderSyncP2P
✅ Headers processed via HeaderSyncP2P::OnHeadersMessage or direct ProcessHeaders
✅ Getheaders requests trigger SendGetheaders callback
✅ Peer lifecycle events wired correctly
✅ State machine driven by Tick()
✅ All Phase N.2 tests still passing
✅ Manual test: headers sync from network

---

## Next Steps

1. ✅ **Plan complete**
2. ⏳ **Implement serializeHeader() helper**
3. ⏳ **Add peer ID conversion maps**
4. ⏳ **Update NetworkManager to use HeaderSyncP2P**
5. ⏳ **Wire callbacks**
6. ⏳ **Test integration**

---

## Conclusion

Phase N.3 is now **simpler than initially planned** because:
- ✅ Header parsing already done (80-byte Bitcoin format)
- ✅ HeaderSyncManager already partially integrated
- ✅ Just need to wrap with HeaderSyncP2P and add callbacks

**Key insight**: The hard work (parsing, validation, state machine) is already done. We're just connecting the pieces.
