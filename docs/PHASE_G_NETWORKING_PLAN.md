# Phase G: Networking Layer - Implementation Plan

**Status:** Ready to begin after Phase E (Economics) completion
**Goal:** Verify, test, and complete DineroCoin's P2P networking layer
**Duration:** 4-6 weeks
**Approach:** Verification-first (test existing code), then implement gaps

---

## Executive Summary

**Existing Code Assessment:**
DineroCoin already has substantial P2P networking infrastructure:
- ✅ P2P manager (`src/daemon/p2p_manager.cpp`)
- ✅ Peer connections (`src/daemon/peer_connection.cpp`)
- ✅ Compact blocks (`src/daemon/p2p/compact_blocks.cpp`) - BIP 152 structure exists
- ✅ Headers-first sync (`src/daemon/p2p/headers_first_sync.cpp`)
- ✅ Address management (`src/p2p/addrman.cpp`)
- ✅ Peer scoring/reputation (`src/daemon/peer_scoring.cpp`)
- ✅ Network message handlers (`src/daemon/network_message_handlers.cpp`)

**What Phase G Must Do:**
1. **Verify** existing networking code with comprehensive tests
2. **Implement** missing pieces (Bloom filters for SPV, if needed)
3. **Integrate** fee estimation with mempool
4. **Optimize** block propagation and sync performance
5. **Harden** against network attacks (DoS, eclipse, Sybil)

**Not:** Build networking from scratch (already exists!)
**Instead:** Test, verify, complete, and harden existing implementation

---

## Phase Breakdown

### **Phase G.1: P2P Protocol Verification** (Week 1)

**Goal:** Verify existing P2P protocol implementation works correctly

**Status Assessment:**
- Existing: P2P manager, peer connections, message handlers
- Need: Comprehensive integration tests

**Tasks:**
1. **P2P Message Protocol Tests**
   - Verify version handshake (version → verack)
   - Test ping/pong keepalive
   - Validate addr/getaddr peer discovery
   - Test inv/getdata block announcement
   - Verify reject messages

2. **Peer Connection Lifecycle Tests**
   - Connection establishment
   - Handshake completion
   - Keepalive behavior
   - Graceful disconnect
   - Timeout handling

3. **Network Message Serialization Tests**
   - Message header format
   - Payload serialization/deserialization
   - Checksum validation
   - Message size limits

**Files to Test:**
- `src/daemon/p2p_manager.cpp`
- `src/daemon/peer_connection.cpp`
- `src/daemon/network_message_handlers.cpp`
- `src/daemon/p2p/messages.cpp`

**Success Criteria:**
- ✅ P2P handshake works (version → verack)
- ✅ Peer discovery works (addr messages)
- ✅ Block announcement works (inv → getdata → block)
- ✅ Connection lifecycle correct (connect → handshake → disconnect)

---

### **Phase G.2: Block Propagation & Sync** (Week 2)

**Goal:** Verify headers-first sync and compact block relay work correctly

**Status Assessment:**
- Existing: Headers-first sync, compact blocks structure
- Need: Integration tests, performance validation

**Tasks:**
1. **Headers-First Sync Verification**
   - Test sync from genesis to tip
   - Verify checkpoint validation
   - Test parallel header download
   - Validate headers-only mode

2. **Compact Blocks (BIP 152) Testing**
   - Verify compact block construction
   - Test short ID collision handling
   - Validate prefilled transaction logic
   - Measure bandwidth savings

3. **Block Relay Performance**
   - Benchmark sync speed (blocks/sec)
   - Measure network utilization
   - Test relay latency (block announcement → full block)
   - Validate compact block efficiency

**Files to Test:**
- `src/daemon/p2p/headers_first_sync.cpp`
- `src/daemon/p2p/compact_blocks.cpp`

**Success Criteria:**
- ✅ Headers-first sync completes from genesis
- ✅ Compact blocks reduce bandwidth (target: 50-90% savings)
- ✅ Sync performance acceptable (target: >100 blocks/sec on fast connection)
- ✅ No stalls or hangs during sync

---

### **Phase G.3: Bloom Filters (BIP 37) - SPV Support** (Week 3)

**Goal:** Implement or verify Bloom filter support for SPV wallets

**Status Assessment:**
- Existing: **NOT FOUND** (likely needs implementation)
- Need: Full BIP 37 implementation + tests

**Tasks:**
1. **Bloom Filter Implementation**
   - Implement `CBloomFilter` class
   - Support `filterload`, `filteradd`, `filterclear` messages
   - Implement filtered block relay (`merkleblock` message)
   - Support transaction matching

2. **BIP 37 Message Handlers**
   - Handle `filterload` (set bloom filter)
   - Handle `filteradd` (add element to filter)
   - Handle `filterclear` (remove filter)
   - Send `merkleblock` for filtered blocks

3. **SPV Wallet Testing**
   - Test wallet can set bloom filter
   - Verify only matching transactions relayed
   - Test merkle proof validation
   - Measure false positive rate

**Files to Create:**
- `include/p2p/bloom_filter.h`
- `src/p2p/bloom_filter.cpp`
- `tests/p2p/test_bloom_filters.cpp`

**Success Criteria:**
- ✅ SPV wallet can set bloom filter
- ✅ Only matching transactions relayed
- ✅ Merkle proofs validate correctly
- ✅ False positive rate < 1% (configurable)

---

### **Phase G.4: Fee Estimation Integration** (Week 4, Days 1-3)

**Goal:** Integrate fee estimation with mempool and wallet

**Status Assessment:**
- Existing: Mempool fee tracking (F.9), no estimator
- Need: Fee estimator implementation + RPC integration

**Tasks:**
1. **Fee Estimator Implementation**
   - Track fee rates of confirmed transactions
   - Calculate fee estimates for N-block confirmation targets
   - Implement exponential moving average
   - Handle fee rate volatility

2. **Mempool Integration**
   - Track time-to-confirm for transactions
   - Update estimates on block arrival
   - Persist estimates across restarts

3. **RPC Integration**
   - Implement `estimatefee` RPC command
   - Implement `estimatesmartfee` (with conf target)
   - Return fee estimate + confidence level

**Files to Create:**
- `include/mempool/fee_estimator.h`
- `src/mempool/fee_estimator.cpp`
- `tests/mempool/test_fee_estimation.cpp`

**Success Criteria:**
- ✅ Fee estimates track actual confirmation times
- ✅ Estimates converge within 10% of optimal fee
- ✅ RPC commands return reasonable estimates
- ✅ Estimates persist across node restarts

---

### **Phase G.5: Peer Discovery & Address Management** (Week 4, Days 4-5)

**Goal:** Verify peer discovery and address management work correctly

**Status Assessment:**
- Existing: Address manager (`addrman.cpp`)
- Need: Verification tests

**Tasks:**
1. **Address Manager Tests**
   - Test address storage (new, tried buckets)
   - Verify address selection algorithm
   - Test peer banning logic
   - Validate address serialization

2. **Peer Discovery Tests**
   - Test DNS seed bootstrap
   - Verify manual addnode
   - Test peer crawling (addr messages)
   - Validate address broadcast

3. **Peer Scoring Tests**
   - Verify reputation system
   - Test ban logic (misbehavior score)
   - Validate automatic unbanning
   - Test prioritization of good peers

**Files to Test:**
- `src/p2p/addrman.cpp`
- `src/daemon/peer_scoring.cpp`
- `src/daemon/peer_reputation_db.cpp`

**Success Criteria:**
- ✅ Node discovers peers automatically
- ✅ Address manager maintains diverse peer set
- ✅ Misbehaving peers get banned
- ✅ Good peers get prioritized

---

### **Phase G.6: Network Hardening** (Week 5)

**Goal:** Harden network layer against attacks

**Tasks:**
1. **DoS Protection**
   - Rate limit incoming connections
   - Limit message sizes
   - Prevent resource exhaustion
   - Implement backpressure

2. **Eclipse Attack Prevention**
   - Diversify peer selection (IP ranges, ASNs)
   - Implement outbound-only connections
   - Validate block announcements from multiple peers
   - Test with adversarial peers

3. **Sybil Resistance**
   - Limit connections per IP range
   - Implement proof-of-work for peer connections (optional)
   - Test with many fake peers

**Files to Test:**
- `src/daemon/p2p_manager.cpp` (connection limits)
- `src/daemon/peer_scoring.cpp` (ban logic)

**Success Criteria:**
- ✅ Node resists connection flooding
- ✅ Eclipse attacks fail (need 10+ good peers)
- ✅ Sybil attacks don't dominate peer set
- ✅ Resource usage bounded under attack

---

### **Phase G.7: Pruning Execution** (Week 6, Optional)

**Goal:** Implement block pruning to reduce disk usage

**Status Assessment:**
- Existing: Pruning invariants (F.7), no execution
- Need: Pruning logic implementation

**Tasks:**
1. **Pruning Implementation**
   - Delete old block files (blk*.dat, rev*.dat)
   - Maintain minimum 288 blocks (F.7 invariant)
   - Update block index (mark blocks as pruned)
   - Handle reorg with pruned blocks

2. **Pruning Configuration**
   - Add `-prune=<MB>` flag
   - Implement manual pruning RPC
   - Add pruning status to getblockchaininfo

3. **Pruning Tests**
   - Test pruning doesn't delete recent blocks
   - Verify reorg works with pruned chain
   - Test wallet still functions (can't rescan)

**Files to Create:**
- `src/storage/block_pruner.cpp`
- `tests/storage/test_block_pruning.cpp`

**Success Criteria:**
- ✅ Pruning reduces disk usage (target: 80-90% savings)
- ✅ Node still validates new blocks
- ✅ Reorgs work (if within non-pruned window)
- ✅ Wallet functions correctly

---

## Implementation Timeline

**Total Duration:** 4-6 weeks

### **Week 1: P2P Protocol Verification**
- Days 1-3: Message protocol tests
- Days 4-5: Peer connection tests

### **Week 2: Block Propagation**
- Days 1-3: Headers-first sync tests
- Days 4-5: Compact blocks tests

### **Week 3: Bloom Filters (BIP 37)**
- Days 1-3: Bloom filter implementation
- Days 4-5: SPV wallet testing

### **Week 4: Fee Estimation + Peer Discovery**
- Days 1-3: Fee estimator implementation
- Days 4-5: Peer discovery verification

### **Week 5: Network Hardening**
- Days 1-3: DoS protection
- Days 4-5: Eclipse/Sybil resistance

### **Week 6: Pruning (Optional)**
- Days 1-3: Pruning implementation
- Days 4-5: Pruning tests + integration

---

## Success Criteria

After Phase G completion, DineroCoin will have:

### **P2P Networking**
- ✅ Stable peer connections
- ✅ Efficient block propagation (compact blocks)
- ✅ Fast sync (headers-first)
- ✅ SPV wallet support (bloom filters)

### **Fee Market**
- ✅ Fee estimation integrated
- ✅ Wallet can suggest optimal fees
- ✅ Estimates track actual confirmation times

### **Security**
- ✅ DoS resistant (rate limits, message size limits)
- ✅ Eclipse attack resistant (diverse peer set)
- ✅ Sybil resistant (connection limits per IP)

### **Efficiency**
- ✅ Pruning support (80-90% disk savings)
- ✅ Compact block bandwidth savings (50-90%)
- ✅ Fast sync (>100 blocks/sec)

---

## What's NOT Included (Out of Scope)

**Explicitly deferred:**
- Lightning Network integration (Phase L)
- Stratum mining protocol (separate phase)
- Advanced relay policies (transaction packages, etc.)

---

## Testing Strategy

**Phase G is verification-heavy:**
1. Test existing P2P code extensively
2. Create integration tests (multi-node scenarios)
3. Implement missing pieces (bloom filters)
4. Performance benchmarks (sync speed, bandwidth)
5. Attack simulations (DoS, eclipse, Sybil)

**Test Infrastructure:**
- Multi-node test harness (3-5 nodes)
- Network simulator (latency, packet loss)
- Adversarial peer simulator
- Performance profiler

---

## After Phase G: What's Next?

**Phase L: Lightning Network**
- Channel lifecycle
- HTLC routing
- Watchtowers

**Or:**

**Phase H: Wallet Improvements**
- HD wallet (BIP 32/39/44)
- PSBT support (BIP 174)
- Hardware wallet integration

---

## Notes

**Key Difference from Economics:**
- Economics was pure validation (no external dependencies)
- Networking is integration-heavy (peers, sockets, protocols)
- Testing requires multi-node scenarios, not just unit tests

**Approach:**
- Verify first (test existing code)
- Fill gaps (implement missing pieces like bloom filters)
- Harden (DoS protection, attack resistance)
- Optimize (compact blocks, fast sync)
