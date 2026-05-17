# Phase N → P2PService Migration Plan

> Historical note as of 2026-03-09:
> The migration described here is complete enough that the active daemon path is
> `P2PService -> P2PManager`, with `NetworkManager` removed from the live build.
> Keep this file as design history. For the current state, prefer the code and the
> integration tests around `P2PService`, compact-block relay, and restart/churn soak.

## Date: 2025-12-21
## Goal: Integrate Phase N components into production P2PService architecture

---

## Current Architecture

### Production (P2PService)
```
P2PService (in DaemonContext)
  ├─ Callbacks: OnHeaders, OnNewBlock, OnInv, OnGetData
  ├─ Currently wired to: ChainstateService
  └─ Located: src/daemon/services/p2p_service.cpp

DaemonApp initialization (daemon_app.cpp):
  1. Init P2PService
  2. Wire callbacks: P2PService → ChainstateService
  3. Start P2PService
```

### Current Runtime (2026-03-09)
```
P2PService
  └─ Owns P2PManager
       ├─ live peer control
       ├─ header / block routing
       ├─ compact-block negotiation
       └─ network invariants

ChainstateService
  ├─ validated header/network-height view
  ├─ block download + reorg control
  └─ CSN / bridge sync handling
```

---

## Migration Strategy

### Design Pattern: **Layered Message Routing**

```
P2PService
  ↓
  ├─ OnHeaders → HeaderSyncP2P (Phase N.3)
  │    └─ Valid headers → HeaderChainSelector
  │         └─ OnHeadersProcessed → BlockDownloadScheduler
  │
  ├─ OnNewBlock → BlockDownloadScheduler (Phase N.4)
  │    └─ Validated blocks → Storage (no activation yet)
  │         └─ Phase N.5 will activate later
  │
  └─ OnInv, OnGetData → ChainstateService (unchanged)
```

**Key Principle:** Headers/blocks flow through Phase N first, then to chainstate (later phase).

---

## Implementation Steps

### Step 1: Add Phase N Components to DaemonContext

**File:** `include/daemon/daemon_context.h`

Add after line 173 (lightning):
```cpp
// Phase N: Headers-first sync and block download
namespace consensus {
class HeaderSyncP2P;
class BlockDownloadScheduler;
class HeaderChainSelector;
class HeaderStore;
}

struct DaemonContext {
    // ... existing members ...

    // Phase N: Headers-first blockchain synchronization
    std::shared_ptr<dinero::consensus::HeaderChainSelector> header_chain;
    std::shared_ptr<dinero::consensus::HeaderStore> header_store;
    std::shared_ptr<dinero::consensus::HeaderSyncP2P> header_sync;
    std::shared_ptr<dinero::consensus::BlockDownloadScheduler> block_download;
};
```

**Justification:**
- HeaderChainSelector: Core header validation and fork choice
- HeaderStore: Persistent header storage
- HeaderSyncP2P: P2P protocol layer for headers
- BlockDownloadScheduler: Block download orchestration

---

### Step 2: Initialize Components in DaemonApp

**File:** `src/daemon/daemon_app.cpp`

**Location:** After chainstate initialization, before P2P service start

```cpp
// Phase N: Initialize headers-first sync
std::cout << "[DaemonApp] Phase N: Headers-first blockchain sync" << std::endl;

// Create HeaderStore (persistent storage)
auto header_store = std::make_shared<HeaderStore>(datadir + "/headers");
if (!header_store->Open()) {
    std::cerr << "[DaemonApp] Failed to open header store" << std::endl;
    return false;
}
ctx_.header_store = header_store;

// Create HeaderChainSelector (consensus logic)
auto header_chain = std::make_shared<HeaderChainSelector>(header_store.get());
ctx_.header_chain = header_chain;

// Create HeaderSyncP2P (P2P protocol layer)
auto header_sync = std::make_shared<HeaderSyncP2P>(header_chain.get());
ctx_.header_sync = header_sync;

// Create BlockDownloadScheduler (block download orchestration)
auto block_download = std::make_shared<BlockDownloadScheduler>(header_chain.get());
ctx_.block_download = block_download;

std::cout << "[DaemonApp] ✅ Phase N components initialized" << std::endl;
```

---

### Step 3: Wire P2PService Callbacks to Phase N

**File:** `src/daemon/daemon_app.cpp`

**Location:** In the P2P callback wiring section

**Replace:**
```cpp
// OLD: Direct routing to chainstate
p2p_service->OnHeaders = [chainstate_service](...) {
    chainstate_service->OnHeaders(...);
};
```

**With:**
```cpp
// NEW: Route through Phase N first
p2p_service->OnHeaders = [header_sync = ctx_.header_sync](
    const std::string& peer_addr,
    const ::P2PMessage& msg
) {
    // Convert peer address to peer ID (simple hash for now)
    uint64_t peer_id = std::hash<std::string>{}(peer_addr);

    // Parse headers from message payload
    std::vector<BlockHeader> headers = ParseHeadersFromP2PMessage(msg);

    // Route to HeaderSyncP2P
    if (header_sync) {
        header_sync->GetSyncManager()->ProcessHeaders(peer_id, headers);
    }
};

p2p_service->OnNewBlock = [block_download = ctx_.block_download](
    const std::string& peer_addr,
    const ::P2PMessage& msg
) {
    // Deserialize block from payload
    Block block = DeserializeBlockFromP2PMessage(msg);

    // Route to BlockDownloadScheduler
    if (block_download) {
        block_download->OnBlockReceived(block);
    }
};
```

---

### Step 4: Wire Phase N Callbacks Back to P2PService

**HeaderSyncP2P needs to send getheaders:**

```cpp
// Wire HeaderSyncP2P send callbacks
header_sync->SetSendGetheadersCallback([p2p_service](
    uint64_t peer_id,
    const std::vector<uint256>& locator,
    const uint256& hash_stop
) {
    // Convert peer ID back to address (need peer ID → address mapping)
    std::string peer_addr = GetPeerAddress(peer_id);

    // Create getheaders message
    ::P2PMessage msg = ::P2PMessage::create_getheaders(locator);

    // Send via P2PService
    p2p_service->get().send_to_peer(peer_addr, msg);
});
```

**BlockDownloadScheduler needs to send getdata:**

```cpp
// Wire BlockDownloadScheduler send callbacks
block_download->SetSendGetDataCallback([p2p_service](const uint256& block_hash) {
    // Create getdata message
    std::vector<std::string> hashes = {block_hash.GetHex()};
    ::P2PMessage msg = ::P2PMessage::create_getdata(hashes, "block");

    // Broadcast to connected peers
    p2p_service->get().broadcast_message(msg);
});
```

---

### Step 5: Add Helper Functions

**File:** `src/daemon/daemon_app.cpp` (private section)

```cpp
namespace {

// Helper: Parse headers from P2P message
std::vector<BlockHeader> ParseHeadersFromP2PMessage(const ::P2PMessage& msg) {
    std::vector<BlockHeader> headers;

    // P2PMessage payload is raw bytes
    // Format: [count: varint][header1: 80 bytes][header2: 80 bytes]...

    size_t offset = 0;
    uint64_t count = ReadVarInt(msg.payload, offset);

    for (uint64_t i = 0; i < count && offset + 80 <= msg.payload.size(); i++) {
        BlockHeader header;
        // Deserialize 80-byte header
        DeserializeHeader(header, msg.payload.data() + offset);
        headers.push_back(header);
        offset += 80;
    }

    return headers;
}

// Helper: Deserialize block from P2P message
Block DeserializeBlockFromP2PMessage(const ::P2PMessage& msg) {
    Reader reader(msg.payload);
    Block block;
    Deserialize(reader, block);
    return block;
}

// Helper: Peer ID ↔ Address mapping
std::unordered_map<uint64_t, std::string> g_peer_id_to_addr;
std::unordered_map<std::string, uint64_t> g_peer_addr_to_id;

uint64_t GetPeerID(const std::string& addr) {
    auto it = g_peer_addr_to_id.find(addr);
    if (it != g_peer_addr_to_id.end()) {
        return it->second;
    }
    uint64_t id = std::hash<std::string>{}(addr);
    g_peer_addr_to_id[addr] = id;
    g_peer_id_to_addr[id] = addr;
    return id;
}

std::string GetPeerAddress(uint64_t peer_id) {
    auto it = g_peer_id_to_addr.find(peer_id);
    return (it != g_peer_id_to_addr.end()) ? it->second : "";
}

} // anonymous namespace
```

---

### Step 6: Update CMakeLists.txt

Ensure Phase N source files are in the dinerod target:

```cmake
# Already added in Phase N.4:
src/consensus/header_sync_p2p.cpp
src/consensus/header_sync_manager.cpp
src/consensus/header_chain.cpp
src/consensus/header_store.cpp
src/consensus/block_download_scheduler.cpp
```

---

### Step 7: Create Integration Test

**File:** `tests/integration/test_p2p_phase_n_integration.cpp`

Test that:
1. P2PService receives headers message
2. Routes to HeaderSyncP2P
3. HeaderSyncP2P validates and stores
4. BlockDownloadScheduler requests blocks
5. P2PService receives block message
6. Routes to BlockDownloadScheduler
7. BlockDownloadScheduler validates and stores

---

### Step 8: Deprecate NetworkManager (Future)

After Phase N migration is complete and tested:

1. Remove NetworkManager from build
2. Update unit tests to use P2PService
3. Delete src/daemon/network_manager.cpp
4. Delete include/daemon/network_manager.h

---

## Risk Mitigation

### Risks:
1. **Message format mismatch**: P2PMessage format may differ from NetworkManager format
2. **Peer ID mapping**: HeaderSyncP2P uses uint64_t, P2PService uses string addresses
3. **Callback timing**: Phase N components may be called before fully initialized
4. **Breaking existing chainstate**: Must preserve existing P2P → Chainstate routing

### Mitigations:
1. Add message parsing unit tests
2. Create peer ID ↔ address mapping layer
3. Add initialization order checks in DaemonApp
4. Keep chainstate callbacks intact (just don't call them for headers/blocks)

---

## Testing Strategy

### Unit Tests:
- ✅ Phase N components work in isolation (already passing)
- New: P2PService message parsing
- New: Callback routing logic

### Integration Tests:
- P2PService + HeaderSyncP2P + BlockDownloadScheduler end-to-end
- Regtest: Download headers and blocks from real node
- Verify no chainstate activation

### Regression Tests:
- Existing P2P functionality still works
- ChainstateService still receives inv/getdata messages
- No breaking changes to RPC or wallet

---

## Success Criteria

✅ HeaderSyncP2P receives headers from P2PService
✅ BlockDownloadScheduler receives blocks from P2PService
✅ Phase N components send messages via P2PService
✅ Production daemon can sync headers/blocks from network
✅ All existing tests still pass
✅ No global variables introduced

---

## Timeline Estimate

- Step 1-2: Add to DaemonContext, initialize (30 min)
- Step 3-4: Wire callbacks (1 hour)
- Step 5: Helper functions (30 min)
- Step 6-7: Testing (1 hour)
- Step 8: Cleanup (future)

**Total: ~3 hours for core migration**

---

## Next Actions

1. Implement Step 1: Add to DaemonContext
2. Implement Step 2: Initialize in DaemonApp
3. Implement Step 3-4: Wire callbacks
4. Test integration
5. Verify production build
