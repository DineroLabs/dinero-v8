# Phase W — Wallet & Mining Intelligence

**Status**: 🟢 IN PROGRESS
**Dependencies**: Phase G (Networking & Relay) ✅ COMPLETE

---

## 🎯 Overview

Phase W focuses on the **application layer** - how miners assemble blocks, how wallets perceive sync state, and how humans interact with the node.

**Phase G was about**: How data moves, who to talk to, bandwidth optimization, hostile peer survival
**Phase W is about**: What data to assemble, how miners choose transactions, wallet UX, RPC usability

---

## 🧱 Architecture Boundary

```
┌─────────────────────────────────────────┐
│   Phase G — Networking & Relay          │  ✅ COMPLETE
│   ─────────────────────────────────     │
│   • INV / headers-first / compact       │
│   • Peer intelligence / scoring         │
│   • DoS protection / mempool hints      │
│   • Adaptive thresholds                 │
└─────────────────────────────────────────┘
                    ▼
┌─────────────────────────────────────────┐
│   Phase W — Wallet & Mining Focus       │  ⬅️ YOU ARE HERE
│   ─────────────────────────────────     │
│   • Block assembly intelligence         │
│   • Wallet sync experience              │
│   • RPC ergonomics                      │
│   • Miner ↔ Network feedback            │
└─────────────────────────────────────────┘
```

---

## 📋 Sub-Phases

### W.1: Block Assembly Intelligence
**Goal**: Optimize how miners select and order transactions for maximum fee revenue

**Scope**:
- `BlockAssembler` heuristics with fee-rate optimization
- Ancestor/descendant aware transaction selection
- CPFP (Child Pays For Parent) support
- Compact-aware template construction
- Faster template refresh on mempool delta
- Fee estimation integration

**Depends on**:
- ✅ G.17 (mempool intelligence)
- ✅ G.13 (compact blocks)

**Deliverables**:
1. Enhanced `BlockAssembler` with ancestor scoring
2. CPFP cluster selection
3. Template refresh triggers
4. Fee-rate optimization tests
5. Mining profitability benchmarks

---

### W.2: Wallet Sync UX Improvements
**Goal**: Give users clear visibility into sync state and progress

**Scope**:
- Headers-first progress reporting (% complete, ETA)
- "Catching up" vs "Synced" vs "IBD" state detection
- Orphan / reorg visibility for wallet transactions
- Wallet rescan ergonomics with progress bars
- Block verification progress
- Mempool sync indicators

**Depends on**:
- ✅ G.8 (headers-first sync)
- ✅ G.7 (orphan handling)
- ✅ G.9 (telemetry)
- ✅ G.12 (sync phases)

**Deliverables**:
1. `SyncProgressTracker` class
2. Wallet rescan with progress
3. Reorg notification system
4. IBD/catchup/synced state machine
5. User-facing sync status API

---

### W.3: RPC Ergonomics
**Goal**: Reduce multi-call complexity with aggregated, human-friendly endpoints

**Scope**:
- **`getnodehealth`**: Single call for node health (peers, sync, mempool, disk)
- **`getsyncstate`**: Detailed sync progress (headers, blocks, IBD status, ETA)
- **`getmempoolhint`**: Mempool recommendations (min fee, confirmation time estimates)
- **`getblocktemplateinfo`**: Mining template info without full getblocktemplate
- Aggregated status endpoints
- Human-readable formatting (time remaining, percentages)

**Depends on**:
- ✅ G.9 (telemetry)
- ✅ G.12 (sync phases)
- ✅ G.17 (mempool intelligence)

**Deliverables**:
1. `getnodehealth` RPC
2. `getsyncstate` RPC with ETA
3. `getmempoolhint` RPC
4. `getblocktemplateinfo` RPC
5. RPC documentation with examples

---

### W.4: Miner ↔ Network Feedback Loop (Future)
**Goal**: Close the loop between mining and network propagation

**Scope**:
- Block propagation analytics (how fast our blocks spread)
- Template quality metrics (% of our txs that made it into blocks)
- Mempool synchronization quality
- Peer mining capability detection
- Optimal block size recommendations

**Depends on**:
- ✅ C.1 (block assembly)
- ✅ G.13 (compact blocks)
- ✅ G.17 (mempool intelligence)

**Status**: Optional - implement after C.1-C.3 deliver value

---

## 🎬 Implementation Plan

### Start with C.1 (Fastest Payoff)

**Why C.1 first**:
- Directly impacts miner revenue (fee optimization)
- Leverages existing G.17 mempool intelligence
- Self-contained (minimal external dependencies)
- Immediately measurable (fee revenue benchmarks)

**Recommended order**:
1. **C.1**: Block Assembly Intelligence (2-3 weeks)
2. **C.2**: Wallet Sync UX (1-2 weeks)
3. **C.3**: RPC Ergonomics (1 week)
4. **C.4**: Miner Feedback Loop (optional)

---

## 🔬 C.1 Detailed Breakdown

### W.1.1: Ancestor-Aware Transaction Selection
- Implement ancestor set scoring
- CPFP cluster identification
- Package fee rate calculation
- Ancestor size limits

### W.1.2: Block Template Optimization
- Fee-rate sorted transaction selection
- Compact block aware construction (prefer high-mempool-overlap txs)
- Template caching and incremental updates
- Block weight optimization

### W.1.3: Mempool Delta Triggers
- Detect high-fee transaction arrivals
- Trigger template refresh on significant fee changes
- Avoid unnecessary template rebuilds
- Configurable refresh thresholds

### W.1.4: Fee Estimation Integration
- Use fee estimates to prioritize transactions
- Exclude low-fee transactions early
- Dynamic block size based on fee market
- Revenue optimization vs block propagation trade-off

### W.1.5: Testing & Benchmarks
- Fee revenue comparison tests
- Template generation performance
- CPFP correctness verification
- Block propagation simulation

---

## 📊 Success Metrics

### C.1 Metrics:
- Fee revenue per block (+X% improvement)
- Template generation time (<Xms)
- CPFP accuracy (100% valid clusters)
- Mempool utilization (% of high-fee txs included)

### C.2 Metrics:
- Sync progress accuracy (±5% error)
- ETA accuracy (±10 minutes for IBD)
- User satisfaction (subjective)

### C.3 Metrics:
- RPC call reduction (aggregate endpoints)
- Response time (<100ms for status calls)
- API usability (developer feedback)

---

## 🧪 Testing Strategy

### C.1 Testing:
- Unit tests for ancestor scoring
- Integration tests for block assembly
- Benchmark tests for fee revenue
- Simulation tests for various mempool states

### C.2 Testing:
- Sync state machine tests
- Progress accuracy tests
- Reorg handling tests
- Edge case coverage (empty mempool, IBD, etc.)

### C.3 Testing:
- RPC response format tests
- Performance tests (response time)
- Integration tests with existing RPCs
- Documentation accuracy

---

## 🚀 Getting Started

To begin Phase W.1:

```bash
# 1. Review existing block assembly code
grep -r "BlockAssembler\|CreateNewBlock" src/

# 2. Study mempool transaction selection
grep -r "addPackageTxs\|TestBlockValidity" src/

# 3. Implement ancestor scoring
# See: src/mining/block_assembler.cpp (or equivalent)

# 4. Write tests
# See: tests/mining/ directory
```

---

## 📚 References

- **Bitcoin Core**: `src/node/miner.cpp` (BlockAssembler)
- **BIP 125**: Replace-by-Fee (RBF)
- **CPFP**: Child Pays For Parent
- **Ancestor scoring**: Package-aware fee estimation

---

## ✅ Phase G Completion Summary

Phase G delivered:
- G.1-G.6: Block relay, headers-first, orphan handling
- G.7-G.9: Telemetry, peer intelligence, scheduler integration
- G.10-G.12: Peer scoring, sync phases, download scheduling
- G.13-G.14: Compact blocks, telemetry integration
- G.15: Reorg + compact stress testing
- G.16: Adaptive thresholds + DoS fuzzing
- G.17: Mempool intelligence + fee-aware propagation

**All 37 tests passing** ✅

Phase G is **production-ready** for networking and relay.

---

## 🎯 Next Steps

1. ✅ Review this planning document
2. ⬜ Implement W.1.1 (Ancestor-aware selection)
3. ⬜ Implement W.1.2 (Template optimization)
4. ⬜ Implement W.1.3 (Delta triggers)
5. ⬜ Implement W.1.4 (Fee estimation)
6. ⬜ Implement W.1.5 (Testing & benchmarks)
7. ⬜ Tag Phase W.1 complete
8. ⬜ Proceed to C.2 (Wallet UX)

---

**Phase W — Mining & Wallet Intelligence**
*Building on the solid networking foundation of Phase G*
