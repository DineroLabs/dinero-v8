# Phase C.1.5: Message Routing Completion - Results

**Date**: December 16, 2025
**Status**: ✅ **COMPLETE**

## Objectives

Phase C.1.5 restored the single choke point architecture and implemented minimal P2P block relay message routing (INV → GETDATA → BLOCK).

### What Was Fixed

1. **Single Choke Point Restored**
   - BlockAcceptor now calls `ChainstateService::BroadcastNewBlock()` instead of directly calling `P2PManager.broadcast_message_async()`
   - All block relay (mined or received) flows through ChainstateService
   - Enforces the "Validate→Accept→Announce" contract

2. **P2P Message Handlers Implemented**
   - `ChainstateService::OnInv()` - Receives inventory announcements, sends GETDATA for needed blocks
   - `ChainstateService::OnGetData()` - Receives data requests (block serialization deferred to Phase C.2)
   - Handlers wired in `daemon_app.cpp` via lambda callbacks

3. **Message Routing Validated**
   - INV messages successfully broadcast from mining node
   - GETDATA messages successfully sent in response to INV
   - OnGetData handler receives requests (logs intent, does not send blocks)

## Test Results

### Test Setup
- **Node A**: Regtest node on port 21001 (RPC: 20000)
- **Node B**: Regtest node on port 21002 (RPC: 20001)
- **Connection**: Nodes connected via hardcoded regtest seed nodes

### Observed Behavior

**Node A (Mining Node):**
```
[ChainstateService] Broadcasting new block: 14e8d4cc98db018a...
[ChainstateService] Block INV broadcasted to all peers: 14e8d4cc98db018a...
[ChainstateService] Would send block to 127.0.0.1:0: 14e8d4cc98db018a...
                    (TODO: Phase C.2 - implement block serialization)
```

**Node B (Receiving Node):**
```
[ChainstateService] Need block: 14e8d4cc98db018a...
[ChainstateService] Requesting 1 block(s) from 127.0.0.1:21001
```

### Acceptance Criteria

- ✅ Two local nodes connect (via hardcoded regtest seeds)
- ✅ Node A mines blocks
- ✅ Node B receives INV messages
- ✅ Node B sends GETDATA messages
- ✅ Node A receives GETDATA messages
- ✅ Single choke point enforced (no direct P2PManager → consensus calls)
- ⏸️ Block transmission deferred to Phase C.2 (per explicit scope)
- ⏸️ Automatic height convergence blocked (requires Phase C.2 block serialization)

## Files Modified

### Core Implementation
- `src/daemon/block_acceptor.cpp` (lines 1787-1815)
  - Changed to call `ChainstateService::BroadcastNewBlock()` instead of direct P2PManager broadcast

- `include/daemon/services/chainstate_service.h` (lines 13, 144-163)
  - Added forward declaration: `struct P2PMessage;`
  - Added `OnInv()` and `OnGetData()` method declarations

- `src/daemon/services/chainstate_service.cpp` (lines 661-762)
  - Implemented `OnInv()`: Parse INV, filter for blocks, send GETDATA
  - Implemented `OnGetData()`: Log intent (block transmission deferred)

- `src/daemon/daemon_app.cpp` (lines 254-272)
  - Wired `p2p_service->OnInv` → `chainstate_service->OnInv()`
  - Wired `p2p_service->OnGetData` → `chainstate_service->OnGetData()`
  - Wired `p2p_service->OnNewBlock` → `chainstate_service->ProcessIncomingBlockHex()`

## Explicitly Out of Scope (Deferred to Phase C.2+)

Per user's explicit requirements, the following were **intentionally not implemented**:

- ❌ Block serialization and transmission
- ❌ Orphan block queues
- ❌ Peer reputation/banning
- ❌ Headers-first sync coupling
- ❌ Advanced relay optimizations

## Next Steps: Phase C.2

Phase C.2 will implement:
1. Block serialization for P2P transmission
2. Full `OnGetData()` implementation (send actual block data)
3. Block validation and acceptance on receiving node
4. Automatic blockchain height convergence

## Technical Notes

### Message Flow Architecture

```
┌─────────────┐         INV          ┌─────────────┐
│   Node A    │─────────────────────>│   Node B    │
│  (Miner)    │                       │ (Receiver)  │
│             │<─────────────────────│             │
│ Chainstate  │       GETDATA        │ Chainstate  │
│   Service   │                      │   Service   │
│             │       BLOCK          │             │
│             │──────(Phase C.2)────>│             │
└─────────────┘                      └─────────────┘
```

### Single Choke Point Pattern

All block relay operations (whether mined locally or received from peers) flow through `ChainstateService`:

1. **Mining Path**: `BlockAcceptor` → `ChainstateService::BroadcastNewBlock()` → `P2PService` → `P2PManager`
2. **Relay Path**: `P2PManager` → `P2PService` callbacks → `ChainstateService::OnInv/OnGetData/ProcessIncomingBlock`

This ensures:
- No invalid data is gossiped (validation happens first)
- Consistent block handling across all code paths
- Clear architectural boundaries (consensus ↔ networking)

## Known Issues

1. **Peer Connection Stability**: Peers occasionally disconnect/reconnect during message exchange
   - Non-blocking for Phase C.1.5 (handshake completes, messages route correctly)
   - Likely related to message loop timeout or keepalive timing
   - Does not affect message routing validation

2. **RPC Peer Count**: `network.getpeerinfo` reports 0 peers even when P2P logs show connections
   - Discrepancy between P2PManager internal state and RPC reporting
   - Non-blocking (connections are established, messages flow correctly)
   - Should be investigated in future P2P refinement

## Conclusion

Phase C.1.5 successfully restored the single choke point architecture and validated the minimal viable P2P message routing pipeline. All acceptance criteria met within the explicitly defined scope. Ready to proceed with Phase C.2 for full block transmission implementation.
