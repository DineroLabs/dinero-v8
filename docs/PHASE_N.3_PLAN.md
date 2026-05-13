# Phase N.3: Production P2P Integration - PLAN

**Date**: 2025-12-21
**Status**: Planning
**Prerequisites**: Phase N.2 complete (HeaderSyncP2P tested and ready)

---

## Executive Summary

Phase N.3 integrates the Bitcoin-Core-aligned `HeaderSyncP2P` into the production P2P layer, replacing the existing `MultiPeerHeadersSync` implementation.

**Key Decision**: Replace MultiPeerHeadersSync with HeaderSyncP2P

**Rationale**:
1. ✅ Bitcoin Core uses **single sync peer** strategy, not multi-peer parallel
2. ✅ HeaderSyncP2P implements exact Bitcoin Core timeout formula (15min + 1ms/header)
3. ✅ Phase N.2 locked behavior through 18 comprehensive tests
4. ✅ MultiPeerHeadersSync is a different approach than Bitcoin Core's proven model

---

## Architectural Context

### Current State (Before Phase N.3)

**NetworkManager** uses:
- `MultiPeerHeadersSync` - 4-8 parallel peers, 30s timeout, reputation scoring
- Callback-based message sending
- Message handlers: `handleHeadersMessage()`, `handleGetheadersMessage()`

**HeaderSyncP2P** provides:
- `HeaderSyncManager` state machine (policy)
- Bitcoin Core timeout enforcement (15min + 1ms/header)
- Single sync peer with robust peer switching
- Callback-based architecture (same pattern as MultiPeerHeadersSync)

### Target State (After Phase N.3)

**NetworkManager** uses:
- `HeaderSyncP2P` - Bitcoin Core single-peer strategy
- Same callback pattern (drop-in replacement)
- Enhanced timeout guarantees (tested in Phase N.2)
- Eclipse attack resistance (outbound preference)

---

## Implementation Steps

### Step 1: Replace MultiPeerHeadersSync in NetworkManager

**File**: `include/daemon/network_manager.h`

**Changes**:
```cpp
// Before:
#include "p2p/multi_peer_headers_sync.h"
std::unique_ptr<p2p::MultiPeerHeadersSync> m_headers_sync;

// After:
#include "consensus/header_sync_p2p.h"
std::unique_ptr<consensus::HeaderSyncP2P> m_headers_sync;
```

**File**: `src/daemon/network_manager.cpp` (constructor)

**Changes**:
```cpp
// Before:
m_headers_sync = std::make_unique<p2p::MultiPeerHeadersSync>();
m_headers_sync->setSendMessageCallback([...]);

// After:
m_headers_sync = std::make_unique<consensus::HeaderSyncP2P>(
    chain_selector,  // From blockchain state
    header_store     // Optional persistent storage
);

// Set callbacks
m_headers_sync->SetSendGetheadersCallback([this](
    uint64_t peer_id,
    const std::vector<uint256>& locator,
    const uint256& hash_stop
) {
    // Convert peer_id to peer connection
    auto peer = findPeer(peer_id);
    if (!peer) return;

    // Create getheaders message
    GetheadersMessage msg;
    msg.version = 70001;
    msg.block_locator_hashes = convertLocator(locator);
    msg.hash_stop = hash_stop.GetHex();

    // Send via existing infrastructure
    peer->sendMessage(msg);
});

m_headers_sync->SetDisconnectPeerCallback([this](
    uint64_t peer_id,
    consensus::PeerSwitchReason reason
) {
    // Disconnect peer
    disconnectPeer(peer_id, reason);
});
```

**Complexity**: Low - Same callback pattern, just different types

---

### Step 2: Wire Message Handlers

**File**: `src/daemon/network_message_handlers.cpp`

#### handleHeadersMessage() Integration

**Before** (line 603+):
```cpp
bool NetworkManager::handleHeadersMessage(std::shared_ptr<PeerConnection> peer,
                                          const P2PMessage& message) {
    if (!m_headers_sync) {
        g_logger.warning("MultiPeerHeadersSync not initialized");
        return false;
    }

    // Parse headers...
    // Delegate to m_headers_sync->processHeaders(peer_id, response);
}
```

**After**:
```cpp
bool NetworkManager::handleHeadersMessage(std::shared_ptr<PeerConnection> peer,
                                          const P2PMessage& message) {
    if (!m_headers_sync) {
        g_logger.warning("HeaderSyncP2P not initialized");
        return false;
    }

    // Parse headers message
    const auto& headers_msg = dynamic_cast<const HeadersMessage&>(message);

    // Convert peer connection to peer_id
    uint64_t peer_id = getPeerId(peer);

    // Delegate to HeaderSyncP2P
    bool accepted = m_headers_sync->OnHeadersMessage(peer_id, headers_msg);

    if (!accepted) {
        // Headers were invalid - peer marked as misbehaving
        peer->adjustScore(-10);  // Existing reputation system
    }

    return accepted;
}
```

#### handleGetheadersMessage() Integration

**Before**:
```cpp
bool NetworkManager::handleGetheadersMessage(...) {
    // Current implementation builds response locally
}
```

**After**:
```cpp
bool NetworkManager::handleGetheadersMessage(std::shared_ptr<PeerConnection> peer,
                                             const P2PMessage& message) {
    if (!m_headers_sync) return false;

    const auto& getheaders_msg = dynamic_cast<const GetheadersMessage&>(message);
    uint64_t peer_id = getPeerId(peer);

    // Delegate to HeaderSyncP2P (it will call SetSendHeadersCallback)
    m_headers_sync->OnGetheadersMessage(peer_id, getheaders_msg);

    return true;
}
```

**Complexity**: Low - Simple delegation to HeaderSyncP2P

---

### Step 3: Wire Peer Lifecycle Events

**File**: `src/daemon/network_manager.cpp`

#### Peer Connect

**Location**: `handleVersionMessage()` (line 18-94)

**Add after handshake complete**:
```cpp
// After VERACK received and handshake complete
if (m_headers_sync) {
    uint256 best_hash;
    best_hash.SetHexString(peer->getStartHash());  // From VERSION message

    m_headers_sync->OnPeerConnected(
        peer->getId(),
        peer->getStartHeight(),
        best_hash,
        peer->isOutbound()
    );
}
```

#### Peer Disconnect

**Location**: `cleanupStaleConnections()` or peer disconnect handler

**Add**:
```cpp
void NetworkManager::disconnectPeer(uint64_t peer_id, const std::string& reason) {
    // Notify header sync
    if (m_headers_sync) {
        m_headers_sync->OnPeerDisconnected(peer_id);
    }

    // Existing disconnect logic...
}
```

**Complexity**: Low - Hook into existing lifecycle events

---

### Step 4: Start Header Sync

**File**: `src/daemon/network_manager.cpp`

**Location**: Startup sequence (after peer discovery)

**Add**:
```cpp
void NetworkManager::startHeaderSync() {
    if (!m_headers_sync) return;

    // Check if we need sync (are we behind?)
    auto stats = m_headers_sync->GetStats();

    if (stats.headers_behind > 0) {
        std::cout << "[NetworkManager] Starting header sync ("
                  << stats.headers_behind << " headers behind)" << std::endl;

        m_headers_sync->StartSync();
    }
}
```

**Call from**: `peerMaintenanceThread()` or explicit startup trigger

**Complexity**: Low - Single call to StartSync()

---

### Step 5: Drive State Machine

**File**: `src/daemon/network_manager.cpp`

**Location**: `peerMaintenanceThread()` (runs every 30 seconds)

**Add**:
```cpp
void NetworkManager::peerMaintenanceThread() {
    while (m_running) {
        // Existing maintenance...

        // Drive header sync state machine
        if (m_headers_sync) {
            m_headers_sync->Tick();  // Uses system time by default
        }

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}
```

**Complexity**: Trivial - Single Tick() call

---

### Step 6: Implement Missing TODOs in HeaderSyncP2P

**File**: `src/consensus/header_sync_p2p.cpp`

#### ParseHeadersMessage() (line 203)

**Current**:
```cpp
std::vector<BlockHeader> HeaderSyncP2P::ParseHeadersMessage(const HeadersMessage& msg) {
    // TODO: Proper deserialization
    return {};
}
```

**Implementation**:
```cpp
std::vector<BlockHeader> HeaderSyncP2P::ParseHeadersMessage(const HeadersMessage& msg) {
    std::vector<BlockHeader> headers;
    headers.reserve(msg.headers.size());

    for (const auto& header_data : msg.headers) {
        // Deserialize from raw bytes
        BlockHeader header;
        if (header.Deserialize(header_data)) {
            headers.push_back(header);
        } else {
            std::cerr << "[HeaderSyncP2P] Failed to deserialize header" << std::endl;
            return {};  // Return empty on any parse failure
        }
    }

    return headers;
}
```

#### FindHeadersToSend() (line 220)

**Current**:
```cpp
std::vector<BlockHeader> HeaderSyncP2P::FindHeadersToSend(...) {
    // TODO: Implement header lookup based on locator
    return {};
}
```

**Implementation**:
```cpp
std::vector<BlockHeader> HeaderSyncP2P::FindHeadersToSend(
    const std::vector<std::string>& locator_hashes,
    const std::string& hash_stop,
    size_t max_count
) {
    std::vector<BlockHeader> headers;

    // 1. Find first locator hash that we have
    const HeaderIndexEntry* start_entry = nullptr;
    for (const auto& hash_str : locator_hashes) {
        uint256 hash;
        hash.SetHexString(hash_str);

        start_entry = sync_manager_->GetChainSelector()->LookupHeader(hash);
        if (start_entry) {
            break;  // Found common ancestor
        }
    }

    if (!start_entry) {
        // No common ancestor - start from genesis
        start_entry = sync_manager_->GetChainSelector()->GetGenesisHeader();
    }

    // 2. Walk forward from start_entry, collecting headers
    const HeaderIndexEntry* current = start_entry->next;  // Start after common ancestor

    while (current && headers.size() < max_count) {
        headers.push_back(current->header);

        // Stop at hash_stop if specified
        if (!hash_stop.empty()) {
            uint256 stop_hash;
            stop_hash.SetHexString(hash_stop);

            if (current->header.GetHash() == stop_hash) {
                break;
            }
        }

        current = current->next;
    }

    return headers;
}
```

**Complexity**: Medium - Requires HeaderChainSelector iteration

---

### Step 7: Add Verification Logic (Bitcoin Core Pattern)

**File**: `src/consensus/header_sync_p2p.cpp`

**Location**: `OnPeerSwitchRequested()` when reason == SYNC_COMPLETE

**Current** (line 191-194):
```cpp
if (reason == PeerSwitchReason::SYNC_COMPLETE) {
    std::cout << "[HeaderSyncP2P] Sync complete - TODO: verify with all outbound peers" << std::endl;
    // TODO Phase N.3: Query all outbound peers to verify best chain
}
```

**Implementation**:
```cpp
if (reason == PeerSwitchReason::SYNC_COMPLETE) {
    std::cout << "[HeaderSyncP2P] Sync complete - verifying with outbound peers" << std::endl;

    // Bitcoin Core pattern: Query all outbound peers to verify best chain
    // This prevents accepting a minority chain from a single peer

    if (verify_outbound_callback_) {
        verify_outbound_callback_();
    }
}
```

**Add to HeaderSyncP2P**:
```cpp
using VerifyOutboundCallback = std::function<void()>;
void SetVerifyOutboundCallback(VerifyOutboundCallback callback);
```

**NetworkManager implements**:
```cpp
m_headers_sync->SetVerifyOutboundCallback([this]() {
    // Query all outbound peers for their best header
    for (const auto& [peer_id, peer] : m_peers) {
        if (peer->isOutbound()) {
            // Send getheaders to verify they agree with our best chain
            requestHeadersFromPeer(peer_id);
        }
    }
});
```

**Complexity**: Medium - Requires outbound peer enumeration

---

## Migration Strategy

### Option A: Clean Replacement (RECOMMENDED)

1. Update NetworkManager to use HeaderSyncP2P
2. Remove MultiPeerHeadersSync entirely
3. Update all references in codebase
4. Run existing tests to verify compatibility

**Pros**:
- Clean architecture
- Follows Bitcoin Core model
- All behavior locked by Phase N.2 tests

**Cons**:
- Breaks existing MultiPeerHeadersSync users (if any)

### Option B: Coexistence (NOT RECOMMENDED)

1. Keep both MultiPeerHeadersSync and HeaderSyncP2P
2. Add flag to choose which to use
3. Gradual migration

**Pros**:
- Low risk (can rollback)

**Cons**:
- Code duplication
- Confusion about which to use
- Maintenance burden

**Decision**: Option A (Clean Replacement)

---

## Testing Strategy

### Unit Tests (Already Passing)

Phase N.2 delivered 18 tests that lock behavior:
- ✅ 7 state machine tests (Step 2A)
- ✅ 6 stall simulation tests (Step 2B)
- ✅ 5 P2P integration tests (Step 2C)

### Integration Tests (Phase N.3)

**New tests needed**:
1. NetworkManager → HeaderSyncP2P integration
2. Real message parsing (HeadersMessage → BlockHeader)
3. Peer lifecycle (connect/disconnect) triggers correct HeaderSyncP2P calls
4. Verification logic (sync complete → query outbound peers)

**Test approach**:
- Use existing test infrastructure (bin/test_*)
- Mock PeerConnection for deterministic tests
- Verify callbacks are invoked correctly

### Manual Testing

1. Start dinerod with empty blockchain
2. Connect to peers
3. Verify header sync starts automatically
4. Monitor logs for timeout behavior
5. Verify sync completes and outbound verification occurs

---

## Dependencies

### Required Components

1. ✅ **HeaderChainSelector** - Already exists, validates headers
2. ✅ **HeaderStore** - Already exists (RocksDB persistence)
3. ✅ **HeaderSyncManager** - Already exists (state machine)
4. ✅ **HeaderSyncP2P** - Already exists (P2P wiring)
5. ⏳ **BlockHeader::Deserialize()** - Needs implementation (or exists?)
6. ⏳ **HeaderChainSelector::LookupHeader()** - Needs verification (or exists?)
7. ⏳ **NetworkManager peer ID mapping** - Needs implementation

### Potential Blockers

1. **BlockHeader serialization format**: Need to verify it matches P2P wire format
2. **Peer ID mapping**: uint64_t vs string IDs (MultiPeerHeadersSync uses string)
3. **Thread safety**: NetworkManager has multiple threads, HeaderSyncP2P may need mutex

---

## Timeline Estimate

**Conservative Estimate**: 2-3 days

**Breakdown**:
- Step 1 (Replace in NetworkManager): 2 hours
- Step 2 (Wire message handlers): 3 hours
- Step 3 (Peer lifecycle): 2 hours
- Step 4-5 (Start sync + Tick): 1 hour
- Step 6 (Implement TODOs): 4 hours
- Step 7 (Verification logic): 3 hours
- Testing + debugging: 8 hours
- **Total**: ~23 hours = 3 days

**Optimistic Estimate**: 1-2 days (if dependencies already exist)

---

## Success Criteria

Phase N.3 is complete when:

✅ NetworkManager uses HeaderSyncP2P (not MultiPeerHeadersSync)
✅ Headers messages processed via HeaderSyncP2P
✅ Getheaders messages handled via HeaderSyncP2P
✅ Peer lifecycle events trigger HeaderSyncP2P correctly
✅ Header sync starts automatically when behind
✅ State machine driven by periodic Tick()
✅ ParseHeadersMessage() deserializes headers correctly
✅ FindHeadersToSend() responds to getheaders correctly
✅ Sync completion triggers outbound verification
✅ All Phase N.2 tests still passing
✅ Manual test: dinerod syncs headers from network

---

## Risk Assessment

### Low Risk

- HeaderSyncP2P behavior locked by 18 tests
- Same callback pattern as MultiPeerHeadersSync
- Bitcoin Core-aligned timeout guarantees
- Policy logic already proven in Phase N.2

### Medium Risk

- BlockHeader deserialization may not match wire format
- Peer ID type mismatch (uint64_t vs string)
- Thread safety assumptions

### Mitigation

- Verify BlockHeader serialization format before integration
- Add peer ID conversion layer if needed
- Add mutex to HeaderSyncP2P if NetworkManager is multi-threaded
- Extensive logging during initial testing

---

## Next Steps

1. ✅ **Plan created** (this document)
2. ⏳ **Verify dependencies** (BlockHeader::Deserialize, HeaderChainSelector methods)
3. ⏳ **Implement Step 1** (Replace MultiPeerHeadersSync in NetworkManager)
4. ⏳ **Implement Steps 2-5** (Wire handlers, lifecycle, startup)
5. ⏳ **Implement Step 6** (Complete TODOs in HeaderSyncP2P)
6. ⏳ **Implement Step 7** (Verification logic)
7. ⏳ **Test integration**
8. ⏳ **Manual testing with real network**
9. ⏳ **Document Phase N.3 completion**

---

## Conclusion

Phase N.3 is a **mechanical integration** of already-tested components. The hard work (policy logic, timeout enforcement, stall detection) was completed in Phase N.2.

**Key insight**: Because policy is tested in isolation, wiring becomes safe and predictable.

**Expected outcome**: Bitcoin-Core-grade header sync in production, with timeout guarantees that prevent the edge cases that plagued early Bitcoin Core versions.
