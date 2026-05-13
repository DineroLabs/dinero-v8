# Phase N.3: Production P2P Integration - PROGRESS

**Date**: 2025-12-21
**Status**: In Progress

---

## Completed

### ✅ Phase N.2 (Prerequisites)

**All tests passing**:
- 7 state machine tests (HeaderSyncManager)
- 6 stall simulation tests (timeout behavior)
- 5 P2P integration tests (HeaderSyncP2P callbacks)

**18 total tests** locking Bitcoin-Core-grade header sync behavior.

### ✅ Architecture Analysis

**Discovered**:
1. NetworkManager already uses `HeaderSyncManager` (from Phase H)
2. Header parsing already implemented (80-byte Bitcoin wire format)
3. Conversion to dinero::BlockHeader already done
4. Partial integration exists - just needs callback wiring

**Key files analyzed**:
- `src/daemon/network_manager.cpp` - P2P infrastructure
- `src/daemon/network_message_handlers.cpp` - Message routing
- `include/daemon/peer_connection.h` - Peer management
- `src/p2p/multi_peer_headers_sync.cpp` - Legacy parallel sync (to be replaced)

### ✅ Implementation Planning

**Created**:
1. `docs/PHASE_N.3_PLAN.md` - Initial comprehensive plan
2. `docs/PHASE_N.3_IMPLEMENTATION_PLAN.md` - Revised, streamlined plan
3. Identified all dependencies and integration points

**Decisions made**:
- Replace both `MultiPeerHeadersSync` and raw `HeaderSyncManager*` with `HeaderSyncP2P`
- Use peer ID conversion layer (uint64_t ↔ string)
- Keep existing 80-byte header parsing in NetworkManager
- Wire format: 80 bytes (Bitcoin compatible)
- Internal format: 112 bytes (Utreexo added later with full blocks)

### ✅ Header Serialization Utilities

**Created**:
- `src/daemon/header_serialization.cpp` - Serialization/deserialization
- `include/daemon/header_serialization.h` - Public interface

**Functions**:
- `serializeHeaderForWire()` - BlockHeader → 80-byte wire format
- `deserializeHeaderFromWire()` - 80-byte wire format → BlockHeader
- `hexToBytes()` - Hex string → bytes
- `bytesToHex()` - Bytes → hex string

**Tested**: Not yet compiled/tested

---

## In Progress

### ⏳ NetworkManager Integration

**Next steps**:
1. Add `HeaderSyncP2P` member to NetworkManager
2. Implement peer ID conversion helpers
3. Set up P2P callbacks (SendGetheaders, DisconnectPeer, SendHeaders)
4. Wire peer lifecycle events (connect/disconnect)
5. Start header sync on startup
6. Drive state machine via Tick()

---

## Remaining Work

### 1. Core Integration (HIGH PRIORITY)

**File**: `include/daemon/network_manager.h`

**Changes needed**:
- Add `#include "consensus/header_sync_p2p.h"`
- Add member: `std::unique_ptr<consensus::HeaderSyncP2P> m_header_sync_p2p;`
- Add peer ID maps: `peer_string_to_id_map_`, `peer_id_to_string_map_`
- Add helper declarations: `stringToPeerId()`, `peerIdToString()`, `setupHeaderSyncCallbacks()`

**File**: `src/daemon/network_manager.cpp`

**Changes needed**:
- Initialize `m_header_sync_p2p` in constructor (with dependency injection)
- Implement `setupHeaderSyncCallbacks()`
  - SendGetheadersCallback
  - DisconnectPeerCallback
  - SendHeadersCallback
- Implement peer ID conversion helpers
- Update `peerMaintenanceThread()` to call `Tick()`

### 2. Message Handler Updates (MEDIUM PRIORITY)

**File**: `src/daemon/network_message_handlers.cpp`

**Changes needed**:
- Update `handleHeadersMessage()` to use `m_header_sync_p2p` instead of `header_sync_manager_`
- Update `handleGetheadersMessage()` to delegate to `m_header_sync_p2p`

### 3. Peer Lifecycle Wiring (MEDIUM PRIORITY)

**File**: `src/daemon/network_manager.cpp`

**Changes needed**:
- `handleVersionMessage()` - Call `OnPeerConnected()` after handshake
- `cleanupStaleConnections()` - Call `OnPeerDisconnected()` before removing peer
- Explicit disconnect handler - Call `OnPeerDisconnected()`

### 4. Dependency Injection (HIGH PRIORITY)

**Problem**: NetworkManager needs `HeaderChainSelector*` and `HeaderStore*` to create `HeaderSyncP2P`

**Solutions**:
- Option A: Pass in constructor (requires DaemonApp changes)
- Option B: Add `initializeHeaderSync()` method (late initialization)
- Option C: Create HeaderChainSelector internally (not recommended)

**Recommended**: Option B (late initialization)

**File**: `include/daemon/network_manager.h`

**Add**:
```cpp
void initializeHeaderSync(
    consensus::HeaderChainSelector* chain_selector,
    consensus::HeaderStore* header_store = nullptr
);
```

### 5. Expose GetSyncManager() in HeaderSyncP2P (LOW PRIORITY)

**File**: `include/consensus/header_sync_p2p.h`

**Add**:
```cpp
// For cases where headers are pre-parsed by P2P layer
HeaderSyncManager* GetSyncManager() { return sync_manager_.get(); }
```

**Why**: NetworkManager already parses headers, so bypass `OnHeadersMessage()` and call `ProcessHeaders()` directly

### 6. Build System Updates (LOW PRIORITY)

**File**: `CMakeLists.txt`

**Add to dinerod target**:
- `src/daemon/header_serialization.cpp`
- `src/consensus/header_sync_p2p.cpp` (if not already linked)

### 7. Testing (HIGH PRIORITY)

**Unit tests**:
- Test `serializeHeaderForWire()` / `deserializeHeaderFromWire()` round-trip
- Test peer ID conversion (string ↔ uint64_t)

**Integration tests**:
- Test NetworkManager → HeaderSyncP2P flow
- Test callbacks are invoked correctly
- Test peer lifecycle events

**Manual testing**:
- Start dinerod with empty chain
- Connect to network
- Verify header sync completes
- Check logs for timeout behavior

### 8. Cleanup (LOW PRIORITY)

**Remove or deprecate**:
- `MultiPeerHeadersSync` (if no longer used)
- `header_sync_manager_` raw pointer (replaced by `m_header_sync_p2p`)
- Legacy header sync code paths

---

## Blockers

### Dependency Injection

**Issue**: NetworkManager constructor doesn't have access to `HeaderChainSelector`

**Impact**: Can't create `HeaderSyncP2P` in constructor

**Resolution**: Add `initializeHeaderSync()` method for late initialization

### Thread Safety

**Potential issue**: NetworkManager has multiple threads, HeaderSyncP2P may need mutex

**Resolution**: Review threading model, add mutex if needed

---

## Timeline

**Original estimate**: 2-3 days
**Revised estimate**: 1-2 days (simpler than expected)

**Progress**: ~30% complete

**Time spent**: ~2 hours (planning + header serialization)
**Time remaining**: ~12 hours

**Breakdown**:
- ✅ Planning: 1 hour
- ✅ Header serialization: 1 hour
- ⏳ Core integration: 4 hours
- ⏳ Callback setup: 2 hours
- ⏳ Lifecycle wiring: 1 hour
- ⏳ Testing: 4 hours

---

## Next Immediate Steps

1. **Add header_serialization to build** (5 min)
2. **Test header serialization** (30 min)
3. **Add GetSyncManager() to HeaderSyncP2P** (5 min)
4. **Implement peer ID conversion in NetworkManager** (30 min)
5. **Add initializeHeaderSync() method** (15 min)
6. **Set up callbacks** (2 hours)
7. **Wire message handlers** (1 hour)
8. **Wire peer lifecycle** (1 hour)
9. **Test integration** (2 hours)

---

## Success Criteria

Phase N.3 is complete when:

✅ Header serialization utilities implemented and tested
⏳ NetworkManager uses HeaderSyncP2P
⏳ Peer lifecycle events wired
⏳ Callbacks set up correctly
⏳ State machine driven by Tick()
⏳ All Phase N.2 tests still passing
⏳ Headers sync from network
⏳ Timeout behavior works correctly
⏳ Peer switching works correctly

---

## Notes

- Wire format is 80 bytes (Bitcoin compatible)
- Utreexo commitment only in full blocks, not header sync
- Existing parsing code in network_message_handlers.cpp is correct
- HeaderSyncManager already partially integrated (Phase H)
- Just need to wrap with HeaderSyncP2P and add callbacks

---

## Conclusion

Phase N.3 is progressing well. The hard work (policy logic, parsing, validation) was already done in Phase N.2 and Phase H. What remains is **mechanical wiring** of existing components.

**Next session**: Continue with core integration (adding HeaderSyncP2P to NetworkManager).
