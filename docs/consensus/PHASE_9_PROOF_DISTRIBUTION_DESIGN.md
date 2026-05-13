# Phase 9: Utreexo Proof Distribution & Performance

**Status:** DESIGN (not implemented)
**Consensus Impact:** ❌ NONE (Phase 8 sealed)
**Primary Goal:** Make stateless validation fast, scalable, and practical on real networks

---

## 0. Architectural Principle

**Phase 8 defines correctness. Phase 9 defines efficiency.**

Phase 9 is a CDN + routing + cache layer:
- ✅ Optimize availability, latency, and bandwidth
- ❌ Never change accept/reject behavior
- ❌ Never introduce new consensus fields
- ❌ Never make stateless nodes depend on "trust"

**If a feature risks consensus divergence → it is out of Phase 9.**

---

## 1. Problem Statement

### 1.1 Current State (Post-Phase 8)

Phase 8 enables stateless validation with these guarantees:
- Blocks with valid proofs are accepted
- Blocks with invalid/missing proofs are rejected
- Both stateful and stateless nodes reach identical consensus

**However:**
- No mechanism for proof discovery beyond direct P2P requests
- No proof caching (every block requires fresh proof fetch)
- No routing heuristics (blind peer selection)
- No bandwidth optimization (proofs sent uncompressed)
- Poor latency for stateless nodes (serial request/retry)

### 1.2 Phase 9 Goals (Non-Negotiable)

Phase 9 must make stateless nodes **fast and scalable** without:
- Changing Phase 8 validation semantics
- Adding new consensus rules
- Requiring trust assumptions
- Making proof availability consensus-critical

**Success criteria:**
- Stateless nodes sync as fast as stateful nodes (within 2x)
- Proof cache hit rate >80% during normal operation
- Bandwidth overhead <30% vs stateful nodes
- No new consensus divergence vectors

---

## 2. Phase 9 Subsystems

### 2.1 Proof Gossip (Availability Layer)

**Problem:** Stateless nodes must find proofs without overloading bridge nodes.

**Design Goals:**
- Reduce duplicate requests (many nodes requesting same proof)
- Improve proof availability (distributed proof sources)
- Avoid proof storms (coordinated mass requests)

**Architecture:**

```
┌────────────────────────────────────────────────────────────┐
│                    Proof Gossip Protocol                    │
└────────────────────────────────────────────────────────────┘

1. Bridge node mines/receives block → generates proof
   ↓
2. Broadcast invproof(block_hash, proof_size) to peers
   ↓
3. Interested stateless peers send getproof(block_hash)
   ↓
4. Bridge node responds with proof data
   ↓
5. (Optional) Stateless peer re-gossips invproof to neighbors
```

**Gossip Characteristics:**
- **Best-effort:** No guarantee of delivery
- **TTL-bound:** Max 2 hops to prevent network flooding
- **Non-mandatory:** Missing gossip never causes block rejection
- **Opportunistic:** Piggyback on existing inv/getdata messages where possible

**P2P Messages (Non-Consensus):**

```cpp
// Proof inventory announcement (gossip layer)
struct InvProof {
    uint256 block_hash;      // Block this proof validates
    uint32_t proof_size;     // Size in bytes (for bandwidth planning)
    uint256 proof_hash;      // Hash for deduplication
    uint8_t ttl;             // Remaining gossip hops (max 2)
};

// Proof request (routing layer)
struct GetProof {
    uint256 block_hash;      // Which block's proof
    uint256 expected_root;   // For sanity check (optional)
};

// Proof response (transport layer)
struct ProofData {
    uint256 block_hash;      // Which block
    BlockUtreexoData proof;  // The actual proof (from Phase 7)
};
```

**Critical Rule:**
> Gossip ≠ consensus. Missing gossip must never cause block rejection.
> If gossip fails, stateless nodes fall back to direct requests.

---

### 2.2 Proof Request Routing (Who to Ask)

**Problem:** Blindly asking random peers is inefficient (high latency, wasted bandwidth).

**Routing Heuristics (Non-Consensus):**

```cpp
enum PeerProofCapability {
    UNKNOWN = 0,           // Not yet probed
    BRIDGE_NODE = 1,       // Advertises NODE_UTREEXO_BRIDGE
    STATELESS_NODE = 2,    // May have cached proof
    STATEFUL_FULL = 3,     // Cannot provide proofs
};

// Peer selection strategy (local, non-deterministic)
Peer* SelectProofPeer(uint256 block_hash) {
    // 1. Prefer peers advertising NODE_UTREEXO_BRIDGE
    if (auto bridge = FindBridgePeer(block_hash)) return bridge;

    // 2. Prefer peers with matching chain tip (likely have proof)
    if (auto synced = FindSyncedPeer(block_hash)) return synced;

    // 3. Penalize peers with repeated misses (track miss rate)
    auto candidates = GetPeersExcludingBadActors();

    // 4. Fallback: round-robin among remaining peers
    return PickRandom(candidates);
}

// Peer reputation tracking (non-consensus)
struct PeerProofStats {
    uint32_t requests_sent;
    uint32_t proofs_received;
    uint32_t timeouts;
    uint32_t invalid_proofs;  // Note: invalid = protocol error, not consensus

    double SuccessRate() const {
        return (double)proofs_received / requests_sent;
    }
};
```

**Important Constraints:**
- Routing decisions are **local** (each node makes independent choices)
- Routing decisions are **non-deterministic** (can vary by time/network state)
- Routing decisions are **non-consensus-visible** (cannot affect validation)

**Penalization (Non-Consensus):**
- Timeout → temporary penalty (30 seconds)
- Protocol violation → disconnect
- Invalid proof → disconnect (but block still validated by Phase 8 rules)

---

### 2.3 Proof Cache (Local Performance)

**Problem:** Proofs are expensive to recompute and resend.

**Cache Architecture:**

```cpp
// Cache entry
struct CachedProof {
    uint256 block_hash;           // Primary key
    BlockUtreexoData proof;       // Cached proof data
    uint256 root_hash;            // For quick validation
    uint64_t timestamp;           // For TTL eviction
    uint32_t access_count;        // For LRU eviction
    size_t size_bytes;            // For memory accounting
};

// Cache implementation
class ProofCache {
public:
    // Configuration
    static constexpr size_t MAX_CACHE_SIZE = 1024 * 1024 * 100;  // 100 MB
    static constexpr uint64_t DEFAULT_TTL_SECS = 86400;          // 24 hours

    // Cache operations
    std::optional<BlockUtreexoData> Get(const uint256& block_hash);
    void Put(const uint256& block_hash, const BlockUtreexoData& proof);
    void Evict(EvictionPolicy policy);

private:
    // LRU tracking
    std::unordered_map<uint256, CachedProof> cache_;
    std::list<uint256> lru_order_;
    size_t total_size_bytes_ = 0;

    // Eviction policies
    void EvictLRU();      // Remove least recently used
    void EvictTTL();      // Remove expired entries
    void EvictOldest();   // Remove by timestamp
};
```

**Cache Characteristics:**
- **Key:** `(block_hash)`
- **Value:** `(proof, root_hash, metadata)`
- **Eviction:** LRU + TTL hybrid
- **Storage:** Memory-only (disk optional but non-required)
- **Thread Safety:** Mutex-protected for concurrent access

**Security Rule:**
> **Cached proofs are ALWAYS re-verified before use.**
> Cache is an optimization, not a trust shortcut.

```cpp
bool UseProofFromCache(const uint256& block_hash, BlockUtreexoData& proof) {
    auto cached = proof_cache_.Get(block_hash);
    if (!cached.has_value()) return false;

    // CRITICAL: Re-verify cached proof
    if (!VerifyProof(cached.value(), expected_root)) {
        proof_cache_.Evict(block_hash);  // Corrupted cache entry
        return false;
    }

    proof = cached.value();
    return true;
}
```

**Eviction Strategy:**
1. If `total_size > MAX_CACHE_SIZE`: Evict LRU entries
2. Every 1 hour: Evict TTL-expired entries
3. On memory pressure: Evict oldest entries first

---

### 2.4 Proof Reuse & Deduplication

**Observation:** Many blocks spend overlapping UTXOs (shared ancestry).

**Optimizations (Non-Consensus):**

#### A. Shared Subproof Reuse

```
Block A spends: UTXO_1, UTXO_2, UTXO_3
Block B spends: UTXO_2, UTXO_3, UTXO_4

Proof for Block A contains subproofs for UTXO_2 and UTXO_3.
When constructing proof for Block B, reuse those subproofs.
```

**Implementation:**
```cpp
// Proof fragment cache (bridge nodes only)
struct ProofFragment {
    OutPoint outpoint;         // Which UTXO
    uint256 accumulator_root;  // At which accumulator state
    std::vector<uint256> proof_hashes;  // Merkle path
};

// Deduplication during proof assembly
BlockUtreexoData AssembleProof(const Block& block, UtreexoForest* forest) {
    BlockUtreexoData proof;

    for (const auto& tx : block.vtx) {
        for (const auto& input : tx.vin) {
            // Check fragment cache first
            if (auto fragment = fragment_cache_.Get(input.prevout)) {
                proof.AddFragment(fragment);
            } else {
                // Generate fresh proof from accumulator
                auto new_fragment = forest->GenerateProof(input.prevout);
                proof.AddFragment(new_fragment);
                fragment_cache_.Put(input.prevout, new_fragment);
            }
        }
    }

    return proof.Deduplicate();  // Remove redundant hashes
}
```

#### B. Merkle Path Deduplication

Utreexo proofs contain Merkle paths that often share intermediate nodes:

```
UTXO_A path: [H1, H2, H3, H4, ROOT]
UTXO_B path: [H5, H2, H3, H4, ROOT]
              └─ H2, H3, H4, ROOT are shared
```

**Optimization:** Store shared nodes once, reference by index.

```cpp
struct CompressedProof {
    std::vector<uint256> unique_hashes;  // Deduplicated hash pool
    std::vector<uint16_t> path_indices;  // References into pool
};

// Deduplication algorithm
CompressedProof DeduplicateProof(const BlockUtreexoData& proof) {
    std::unordered_map<uint256, uint16_t> hash_to_index;
    CompressedProof compressed;

    for (const auto& hash : proof.proof_hashes) {
        if (!hash_to_index.count(hash)) {
            hash_to_index[hash] = compressed.unique_hashes.size();
            compressed.unique_hashes.push_back(hash);
        }
        compressed.path_indices.push_back(hash_to_index[hash]);
    }

    return compressed;
}
```

**Strict Constraint:**
> No proof mutation that changes verification semantics.
> Deduplication is purely structural, never cryptographic.

---

### 2.5 Compression (Bandwidth Reduction)

**Goal:** Reduce proof transmission size without changing verification.

**Allowed Techniques:**

#### A. Transport-Layer Compression (Zstd/Brotli)

```cpp
// Compress before sending over network
std::vector<uint8_t> CompressProof(const BlockUtreexoData& proof) {
    auto serialized = SerializeProof(proof);
    return ZstdCompress(serialized);  // Zstd level 3 (fast)
}

// Decompress after receiving
BlockUtreexoData DecompressProof(const std::vector<uint8_t>& compressed) {
    auto decompressed = ZstdDecompress(compressed);
    return DeserializeProof(decompressed);
}
```

**Compression Levels:**
- Zstd level 3: ~60% size reduction, <1ms latency
- Brotli quality 4: ~65% size reduction, ~2ms latency

**Trade-off:** CPU time vs bandwidth. Use Zstd for low-latency, Brotli for mobile.

#### B. Delta Compression (Consecutive Blocks)

```
Block N proof:   [H1, H2, H3, H4, H5]
Block N+1 proof: [H1, H2, H3, H6, H7]
Delta:           [KEEP, KEEP, KEEP, REPLACE(H6), REPLACE(H7)]
```

**Use case:** Stateless node syncing consecutive blocks from same peer.

```cpp
// Delta encoding (optional optimization)
struct ProofDelta {
    uint256 base_block_hash;        // Reference proof
    std::vector<DeltaOp> operations;  // KEEP, REPLACE, INSERT, DELETE
};

DeltaOp = {
    enum Type { KEEP, REPLACE, INSERT, DELETE };
    Type op;
    std::optional<uint256> value;  // For REPLACE/INSERT
};
```

**Not Allowed:**
- ❌ Cryptographic changes (e.g., elliptic curve compression)
- ❌ Proof weakening (e.g., probabilistic verification)
- ❌ Lossy compression (e.g., dropping proof nodes)

---

### 2.6 Lightning Interaction (Read-Only)

**Important Boundary:**
> Phase 9 does NOT change Lightning consensus.
> Lightning reads Utreexo proofs, never writes them.

**Allowed Interactions:**

#### A. HTLC Preimage Verification (Watchtowers)

Watchtowers can operate as stateless nodes:

```cpp
// Watchtower validates HTLC resolution without full UTXO set
bool VerifyHTLCResolution(const Transaction& tx, const BlockUtreexoData& proof) {
    // 1. Extract HTLC preimage from witness
    auto preimage = ExtractPreimage(tx.witness);

    // 2. Verify HTLC spend using Utreexo proof (stateless)
    for (size_t i = 0; i < tx.vin.size(); i++) {
        auto spent_output = proof.spent_outputs[i];

        // Verify this is an HTLC script
        if (!IsHTLCScript(spent_output.scriptPubKey)) continue;

        // Verify preimage hash matches HTLC
        if (SHA256(preimage) != ExtractHashFromHTLC(spent_output.scriptPubKey)) {
            return false;
        }

        // Verify signature (Phase 8 logic, unchanged)
        if (!ValidateSpend(tx, i, spent_output, block_height) == ScriptValidationResult::OK) {
            return false;
        }
    }

    return true;
}
```

#### B. Channel Monitoring (Stateless Watchtowers)

```cpp
// Watchtower monitors channel without UTXO database
class StatelessWatchtower {
public:
    void MonitorChannel(const ChannelOutpoint& channel) {
        // Subscribe to blocks containing channel spends
        SubscribeToProofs(channel.txid);
    }

    void OnBlockReceived(const Block& block, const BlockUtreexoData& proof) {
        // Check if monitored channel was spent
        for (const auto& tx : block.vtx) {
            if (SpendsChannel(tx, monitored_channels_)) {
                // Validate breach remedy using proof (stateless)
                if (IsBreach(tx, proof)) {
                    BroadcastJusticeTx(tx);
                }
            }
        }
    }
};
```

#### C. Faster Channel Startup (Proof Cache)

```cpp
// Lightning node bootstraps from proof cache
bool QuickStartChannel(const OutPoint& funding_outpoint) {
    // Check if funding output proof is cached
    if (auto proof = proof_cache_.GetForOutpoint(funding_outpoint)) {
        // Verify channel is still unspent (stateless check)
        if (VerifyUTXOExists(funding_outpoint, proof)) {
            return true;  // Channel ready immediately
        }
    }

    // Fallback: request proof from bridge node
    return RequestProofAndValidate(funding_outpoint);
}
```

**Explicitly Forbidden in Phase 9:**
- ❌ Covenant enforcement changes
- ❌ Channel rules changes
- ❌ Script semantics changes
- ❌ HTLC timeout modifications
- ❌ Penalty transaction logic changes

---

## 3. Failure Model

Phase 9 introduces **zero new consensus failure modes**.

| Failure Type | Allowed? | Behavior |
|--------------|----------|----------|
| Proof unavailable | ✅ | Retry with different peer |
| Cache miss | ✅ | Request proof from network |
| Gossip failure | ✅ | Ignore, fall back to direct request |
| Slow peer | ✅ | Penalize, select different peer |
| Routing timeout | ✅ | Retry with exponential backoff |
| Compression error | ✅ | Fall back to uncompressed |
| Invalid proof | ❌ | **Reject block (Phase 8 logic)** |

**Critical Invariant:**
> Only Phase 8 logic can reject blocks.
> Phase 9 optimizations never introduce new rejection paths.

---

## 4. Test Strategy (Non-Consensus)

All Phase 9 tests must assert:
- ❌ No new rejection paths
- ❌ No new consensus flags
- ❌ No validation differences between Phase 8 and Phase 9

**Test Categories:**

### 4.1 Proof Cache Tests
- **T9.1:** Cache hit returns correct proof
- **T9.2:** Cache miss triggers network request
- **T9.3:** LRU eviction works correctly
- **T9.4:** TTL eviction removes expired entries
- **T9.5:** Corrupted cache entry triggers re-verification

### 4.2 Routing Tests
- **T9.6:** Bridge nodes preferred over stateless nodes
- **T9.7:** Peer penalization after repeated timeouts
- **T9.8:** Round-robin fallback when bridge nodes unavailable
- **T9.9:** Routing is non-deterministic (same request, different peers)

### 4.3 Gossip Tests
- **T9.10:** Proof gossip propagates to neighbors (best-effort)
- **T9.11:** TTL limit prevents infinite propagation
- **T9.12:** Gossip failure does not block validation
- **T9.13:** invproof deduplication prevents spam

### 4.4 Compression Tests
- **T9.14:** Compressed proof decompresses to original
- **T9.15:** Bandwidth reduction >50% for typical proofs
- **T9.16:** Compression failure falls back to uncompressed

### 4.5 Performance Tests
- **T9.17:** Stateless sync within 2x of stateful sync time
- **T9.18:** Cache hit rate >80% during normal operation
- **T9.19:** Proof request latency <500ms (p95)

### 4.6 Determinism Tests (Critical)
- **T9.20:** Phase 8 and Phase 9 accept identical blocks
- **T9.21:** Phase 8 and Phase 9 reject identical blocks
- **T9.22:** Cache state does not affect validation outcome

---

## 5. What Phase 9 Deliberately Avoids

Phase 9 does **NOT** touch:

| Category | Examples | Rationale |
|----------|----------|-----------|
| **Consensus Rules** | Activation heights, soft forks | Phase 8 sealed consensus |
| **Script Changes** | Opcodes, Taproot semantics | Ring 7/8 frozen |
| **Proof Format** | BlockUtreexoData structure | Phase 7 sealed |
| **Validation Logic** | Accept/reject conditions | Phase 8 sealed |
| **Consensus Flags** | ValidationMode semantics | Phase 8 sealed |
| **Mandatory Features** | Required gossip, required cache | Must remain optional |

**Design Philosophy:**
> Every Phase 9 feature must be **independently removable** without affecting consensus.

---

## 6. Implementation Order (Recommended)

**DO NOT PARALLELIZE THESE.**

Each step must be independently testable and removable.

### Step 9.0: Design Lock ✅
- Write this document
- Get design approval
- No code yet

### Step 9.1: Proof Cache (Local Only)
- Implement `ProofCache` class
- Add LRU + TTL eviction
- Tests: T9.1–T9.5
- **No P2P changes**

### Step 9.2: Routing Heuristics
- Implement peer selection strategy
- Add reputation tracking
- Tests: T9.6–T9.9
- **No gossip yet**

### Step 9.3: Gossip Protocol
- Add `invproof`, `getproof`, `proofdata` messages
- Implement TTL-bound propagation
- Tests: T9.10–T9.13
- **Best-effort only**

### Step 9.4: Compression
- Add Zstd transport compression
- Implement deduplication
- Tests: T9.14–T9.16
- **Optional optimization**

### Step 9.5: Lightning Integration (Read-Only)
- Add watchtower support
- Implement stateless HTLC validation
- **No Lightning consensus changes**

### Step 9.6: Benchmarks & Profiling
- Measure sync time (stateless vs stateful)
- Profile cache hit rates
- Validate bandwidth savings
- Tests: T9.17–T9.19

---

## 7. Success Criteria

Phase 9 is complete when:

1. ✅ Stateless nodes sync within 2x of stateful nodes
2. ✅ Proof cache hit rate >80% in steady state
3. ✅ Bandwidth overhead <30% vs stateful nodes
4. ✅ All Phase 8 tests still pass (T8.1–T8.9)
5. ✅ All Phase 9 tests pass (T9.1–T9.22)
6. ✅ No new consensus divergence vectors introduced

**Most Important:**
> Phase 8 enforcement remains unchanged.
> Phase 9 can be disabled without affecting consensus.

---

## 8. Open Questions (To Be Resolved During Implementation)

1. **Cache Disk Persistence:**
   - Should cache survive daemon restarts?
   - Trade-off: Faster startup vs disk I/O overhead

2. **Proof Pinning:**
   - Should recent blocks be pinned (never evicted)?
   - Protects against cache thrashing during reorgs

3. **Mobile-Specific Optimizations:**
   - Higher compression (Brotli) for mobile peers?
   - Adaptive cache size based on available memory?

4. **Gossip Rate Limiting:**
   - Max proofs per second to prevent DoS?
   - Per-peer vs global rate limits?

5. **Lightning Proof Streaming:**
   - Can watchtowers subscribe to proof streams?
   - Reduces polling overhead for channel monitoring

These will be answered during implementation based on empirical testing.

---

## Appendix A: Bandwidth Analysis

**Typical Proof Sizes (Post-Phase 7):**
- Coinbase-only block: ~32 bytes (empty proof)
- 1-input block: ~500 bytes (single Merkle path)
- 10-input block: ~3 KB (with deduplication)
- 1000-input block: ~200 KB (full block)

**Compression Ratios (Empirical):**
- Zstd level 3: ~60% reduction → 10-input block = 1.2 KB
- Brotli quality 4: ~65% reduction → 10-input block = 1.05 KB

**Bandwidth Overhead (vs Stateful Node):**
- Stateful: Block data only (~1 MB average)
- Stateless (uncompressed): Block + proof (~1.2 MB average) = **+20%**
- Stateless (compressed): Block + compressed proof (~1.08 MB) = **+8%**

**Conclusion:** With compression, stateless nodes use <10% more bandwidth than stateful nodes.

---

## Appendix B: Compatibility Matrix

| Node Type | Phase 8 | Phase 9 | Gossip | Cache | Compression |
|-----------|---------|---------|--------|-------|-------------|
| Bridge Node (Full UTXO) | ✅ Generates proofs | ✅ Serves proofs | Optional | Optional | Optional |
| Stateless Node | ✅ Validates via proofs | ✅ Requests proofs | Optional | Recommended | Recommended |
| Stateful Full Node | ✅ Validates via DB | ❌ No proofs | N/A | N/A | N/A |
| SPV Node | ❌ No validation | ❌ No proofs | N/A | N/A | N/A |

**Backward Compatibility:**
- Phase 9 nodes can sync from Phase 8-only nodes (no gossip, direct requests only)
- Phase 8 nodes unaware of Phase 9 (treats proof requests as normal P2P)
- Stateful nodes unaffected (ignore proof gossip)

---

## Appendix C: Security Considerations

### C.1 DoS Vectors (Non-Consensus)

1. **Proof Request Spam:**
   - **Attack:** Flood bridge node with getproof requests
   - **Mitigation:** Rate limiting (max 100 proofs/sec per peer)

2. **Gossip Amplification:**
   - **Attack:** Broadcast fake invproof to trigger mass requests
   - **Mitigation:** TTL=2 limit, invproof deduplication

3. **Cache Poisoning:**
   - **Attack:** Fill cache with fake proofs
   - **Mitigation:** Always re-verify cached proofs (security rule)

### C.2 Privacy Considerations

1. **Proof Request Fingerprinting:**
   - Requesting specific proofs reveals which UTXOs user cares about
   - **Mitigation:** Request proofs for full blocks, not individual UTXOs

2. **Gossip Traffic Analysis:**
   - invproof patterns reveal network topology
   - **Mitigation:** Random delays, decoy invproof (future work)

---

**End of Phase 9 Design Document**
