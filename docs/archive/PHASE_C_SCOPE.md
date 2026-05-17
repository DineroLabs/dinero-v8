# Phase C: P2P Block Relay and Initial Block Download
**Network Integration with Phase B Consensus**

Date: 2025-12-16
Status: 🔵 **IN PROGRESS**

---

## Overview

Phase C integrates the existing P2P infrastructure with Phase B's consensus layer (ChainManager, BlockAcceptor, UTXO set) to enable:
- Block relay across the network
- Header-first synchronization
- Initial Block Download (IBD)
- Network-wide consensus convergence

---

## 🔒 Scope Lock (Authoritative)

### ✅ Allowed Operations

1. **P2P → Consensus Wiring**
   - Connect P2PService to ChainstateService
   - Wire incoming blocks to BlockAcceptor
   - Wire block acceptance to P2P broadcast

2. **Header-First Sync**
   - Implement GETHEADERS / HEADERS message handling
   - Download headers before blocks
   - Validate header chain (PoW, timestamps)

3. **Block Relay**
   - Receive blocks from peers via INV / GETDATA
   - Validate blocks using BlockAcceptor
   - Broadcast accepted blocks to other peers
   - Handle orphan blocks (missing parent)

4. **Initial Block Download (IBD)**
   - Detect IBD state (local tip << network tip)
   - Request blocks from best peer
   - Batch block downloads
   - Progress tracking

5. **Network Consensus**
   - All nodes converge on same best chain
   - Reorgs propagate across network
   - Fork resolution via chainwork

### ❌ Forbidden Operations

1. **No Consensus Changes**
   - ❌ Do NOT modify fork choice (Phase A is frozen)
   - ❌ Do NOT change reorg execution (Phase A is frozen)
   - ❌ Do NOT alter UTXO validation (Phase B is frozen)
   - ❌ Do NOT touch persistence logic (Phase B.3 is frozen)

2. **No Protocol Changes**
   - ❌ Do NOT change P2P message format (already defined)
   - ❌ Do NOT add new consensus rules
   - ❌ Do NOT modify block validation rules

3. **No Performance Optimizations**
   - ❌ Defer compact blocks, header announcements, etc.
   - ❌ Focus on correctness, not speed
   - ❌ Performance tuning is post-Phase C work

4. **No Security Hardening (Yet)**
   - ❌ DoS protection is Phase D work
   - ❌ Peer banning is Phase D work
   - ❌ Assume honest peers for Phase C

---

## Architecture Analysis

### Existing Components (Already Built)

**P2P Layer** (`src/daemon/p2p_*.cpp`, `include/dinero/daemon/p2p_*.h`):
- ✅ `P2PMessage` - Message serialization/deserialization
- ✅ `P2PConnection` - Socket management, read/write loops
- ✅ `P2PNetwork` - Connection management, peer discovery
- ✅ `P2PService` - Daemon service wrapper
- ✅ All standard Bitcoin P2P messages (VERSION, INV, GETDATA, BLOCK, etc.)

**Consensus Layer** (Phase B - Frozen):
- ✅ `ChainManager` - Fork choice, reorgs, UTXO state machine
- ✅ `BlockAcceptor` - Block validation, block application
- ✅ `ChainDB` - Persistent storage (headers, blocks, UTXOs, undo)
- ✅ `UTXOSet` - In-memory UTXO set

**Services** (`src/daemon/services/`):
- ✅ `ChainstateService` - Owns ChainDB, ChainManager, BlockAcceptor
- ✅ `P2PService` - Owns P2PManager
- ✅ `MempoolService` - Transaction relay (already wired)

### Missing Integrations (Phase C Work)

**1. Block Relay Path**
```
[Peer] --INV--> [P2PService] --?--> [ChainstateService] --?--> [BlockAcceptor]
                                                                       ↓
                                                               [ChainManager::ProcessNewBlock]
                                                                       ↓
                                                               [UTXO validation]
                                                                       ↓
                                                               [Accept / Reject]
                                                                       ↓
[Peers] <--INV-- [P2PService] <--?-- [ChainstateService] <-- [Broadcast if accepted]
```

**Current State**: P2PService receives blocks but doesn't route them to BlockAcceptor.

**Phase C Goal**: Complete the `--?-->` connections.

**2. Header-First Sync**
```
[Node startup] --> [Request headers: GETHEADERS]
                           ↓
                   [Receive: HEADERS]
                           ↓
                   [Validate header chain]
                           ↓
                   [Request blocks: GETDATA]
                           ↓
                   [Apply blocks via BlockAcceptor]
```

**Current State**: GETHEADERS/HEADERS messages defined but not handled.

**Phase C Goal**: Implement header-first sync logic.

**3. Initial Block Download (IBD)**
```
[Detect IBD] (local tip << peer tips)
      ↓
[Select best peer] (highest announced height)
      ↓
[Request block batch] (GETDATA with 500 block hashes)
      ↓
[Receive blocks] (BLOCK messages)
      ↓
[Apply sequentially] (BlockAcceptor)
      ↓
[Repeat until synced]
```

**Current State**: No IBD logic.

**Phase C Goal**: Implement IBD state machine.

---

## Phase C Sub-Phases

### Phase C.1: Block Relay (Foundation)
**Goal**: Single block can propagate from one node to another

**Tasks**:
1. Wire P2PService::handleBlock() → ChainstateService::ProcessIncomingBlock()
2. ChainstateService calls BlockAcceptor::AcceptBlock()
3. BlockAcceptor calls ChainManager::ProcessNewBlock()
4. On success, ChainstateService broadcasts INV to other peers
5. Handle orphan blocks (queue until parent arrives)

**Success Criteria**:
- Node A mines block → broadcasts to Node B → Node B accepts and stores block
- Orphan block handling works (out-of-order blocks)

**Test**: 2-node network, mine block on one node, verify it appears on the other

### Phase C.2: Header-First Sync
**Goal**: Download headers before downloading full blocks

**Tasks**:
1. Implement GETHEADERS message creation (block locator algorithm)
2. Implement HEADERS message handling (validate PoW, difficulty, timestamps)
3. Store headers in ChainDB before downloading blocks
4. Request blocks only after header chain validated

**Success Criteria**:
- Node downloads 1000 headers in < 5 seconds
- Node validates header chain before requesting blocks
- Invalid headers rejected (bad PoW, bad timestamp)

**Test**: Fresh node syncs headers from node with 1000 blocks

### Phase C.3: Initial Block Download (IBD)
**Goal**: Fresh node can catch up from genesis to network tip

**Tasks**:
1. Detect IBD state (local height < peer height - 144 blocks)
2. Select best sync peer (highest height, lowest latency)
3. Request blocks in batches (500 blocks per GETDATA)
4. Apply blocks sequentially via BlockAcceptor
5. Track IBD progress (log every 1000 blocks)

**Success Criteria**:
- Fresh node syncs 10,000 blocks
- IBD completes without stalling
- Node reaches network tip within reasonable time

**Test**: Fresh node syncs from archival node with 10k blocks

### Phase C.4: Network Consensus
**Goal**: All nodes converge on same best chain, reorgs propagate

**Tasks**:
1. Verify reorgs trigger block relay (new tip → INV broadcast)
2. Verify competing chains converge (all nodes choose highest chainwork)
3. Handle network splits (nodes rejoin and reorg to common chain)

**Success Criteria**:
- 3-node network with competing chains → all converge to same tip
- Reorg on one node → other nodes follow
- Network split + rejoin → nodes converge

**Test**: 3-node test with deliberate fork, verify convergence

---

## Implementation Plan

### Week 1: Phase C.1 (Block Relay)
**Days 1-2**: Wire P2P → Chainstate → BlockAcceptor
**Days 3-4**: Orphan block handling
**Day 5**: Testing (2-node relay test)

### Week 2: Phase C.2 (Header-First Sync)
**Days 1-2**: GETHEADERS / HEADERS implementation
**Days 3-4**: Header validation (PoW, difficulty, timestamps)
**Day 5**: Testing (header sync test)

### Week 3: Phase C.3 (IBD)
**Days 1-2**: IBD state detection and peer selection
**Days 3-4**: Batch block download logic
**Day 5**: Testing (full 10k block sync)

### Week 4: Phase C.4 (Network Consensus)
**Days 1-2**: Reorg propagation testing
**Days 3-4**: Multi-node convergence testing
**Day 5**: Integration testing (all Phase A + B + C tests)

---

## Success Criteria (Phase C Complete)

### Functional Requirements
- ✅ Single block relay works (A → B)
- ✅ Header-first sync works (download headers before blocks)
- ✅ IBD works (fresh node syncs from genesis)
- ✅ Network consensus works (all nodes converge)
- ✅ Reorgs propagate (one node reorgs → others follow)

### Test Coverage
- ✅ 2-node block relay test
- ✅ Header-first sync test (1000 headers)
- ✅ IBD test (10k blocks)
- ✅ 3-node convergence test
- ✅ Network split + rejoin test

### Integration
- ✅ All Phase A tests still pass (reorg machinery)
- ✅ All Phase B tests still pass (UTXO persistence)
- ✅ P2P layer successfully uses Phase B consensus

---

## Dependencies

### Phase A (Reorg Machinery) - FROZEN ✅
- Fork choice algorithm
- Reorg execution
- Mempool reconciliation

### Phase B (UTXO Subsystem) - FROZEN ✅
- UTXO state machine
- Block validation
- Persistence

### Existing P2P Infrastructure - AVAILABLE ✅
- Message protocol
- Connection management
- Peer discovery

---

## Risk Analysis

### Low Risk ✅
- **P2P infrastructure mature**: Messages, connections, peer management all exist
- **Consensus layer frozen**: No changes needed to Phase A/B
- **Clear integration points**: P2PService → ChainstateService → BlockAcceptor

### Medium Risk ⚠️
- **Orphan handling complexity**: Blocks arriving out-of-order
- **IBD performance**: Need to batch efficiently without stalling
- **Peer selection**: Choosing best sync peer (least critical for Phase C)

### High Risk ❌ (None identified)

---

## Open Questions

1. **Orphan block storage**: Where to queue blocks waiting for parent?
   - **Option A**: In-memory queue in ChainstateService (simpler)
   - **Option B**: Persist to disk in ChainDB (crash-safe)
   - **Recommendation**: Option A for Phase C, Option B for production

2. **IBD batch size**: How many blocks per GETDATA?
   - Bitcoin uses 128-500 blocks per request
   - **Recommendation**: Start with 500, tune later

3. **Header checkpoint**: Should we hardcode a recent block hash?
   - **Recommendation**: Not for Phase C (adds complexity)
   - **Future work**: Add checkpoints in Phase D (security hardening)

---

## Non-Goals (Explicitly Deferred)

- ❌ Compact blocks (BIP152)
- ❌ Header announcements (sendheaders)
- ❌ DoS protection / peer banning
- ❌ Performance optimization (parallel block download)
- ❌ UTXO commitments / assumevalid
- ❌ Pruning (already exists, don't touch)

These are post-Phase C improvements.

---

## Commit Strategy

**Phase C.1**: `git commit -m "Phase C.1: Wire P2P block relay to BlockAcceptor"`
**Phase C.2**: `git commit -m "Phase C.2: Implement header-first sync"`
**Phase C.3**: `git commit -m "Phase C.3: Implement Initial Block Download"`
**Phase C.4**: `git commit -m "Phase C.4: Multi-node network consensus tests"`
**Final Tag**: `git tag -a v0.16.0.0-phase-c-complete -m "Phase C complete: P2P integration with consensus layer"`

---

## Conclusion

Phase C is **network integration**, not protocol design. The hard work (consensus, UTXO, persistence) is done. Phase C connects the pieces.

**Estimated Effort**: 2-3 weeks
**Complexity**: Medium (integration, not invention)
**Risk**: Low (clear architecture, frozen dependencies)

**Next Phase**: Phase D (Consensus Rule Hardening) or Phase E (Production Hardening)

---

**Phase C Status**: 🔵 **READY TO BEGIN**
