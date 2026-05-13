# Phase 12: Mobile / Embedded Deployment Profile

**Status**: Configuration layer
**Consensus impact**: ❌ None
**Code impact**: Minimal, isolated, reversible

---

## Executive Summary

Phase 12 is **not a new subsystem** — it is a profiled deployment mode built entirely on top of Phases 8–11. Nothing new becomes consensus-critical.

A mobile node validates blocks **exactly the same way** as a full node — it just retains less data and asks more often.

---

## 1. What Phase 12 Is (and Is Not)

### What It Is
- A **constrained-runtime configuration** of the existing stateless node stack
- A **resource envelope** wrapped around Phases 8-11
- A set of **default parameters** optimized for mobile/embedded devices

### What It Is NOT
- ❌ New validation rules
- ❌ New proof formats
- ❌ New network messages
- ❌ New trust assumptions
- ❌ A "light client"

---

## 2. Foundation Recap (Why Phase 12 Is Even Possible)

Phase 12 exists only because these are already sealed:

| Phase | What it unlocked |
|-------|------------------|
| Phase 8 | Stateless validation (no UTXO DB) |
| Phase 9 | Proof distribution, caching, compression |
| Phase 10 | Real-world sync stability |
| Phase 11 | Lightning as a pure client |

So **Phase 12 is configuration + tuning, not invention**.

---

## 3. Core Principle

> A mobile node is not a light client — it is a **constrained full validator**.

**Same rules. Same security. Different resource envelope.**

This is architecturally rare. Most chains do:
```
Full node → light client → trust
```

DineroCoin does:
```
Full node → stateless node → constrained stateless node
```

---

## 4. Architectural Hierarchy

```
Phase 8:  Stateless validation (foundation)
Phase 9:  Proof distribution (infrastructure)
Phase 10: Sync validation (robustness)
Phase 11: Lightning client (application)
Phase 12: Mobile profile (deployment mode)
          ↑
          Configuration of Phases 8-11
```

**There is no "Phase 12 engine"** — just different defaults fed to existing systems.

---

## 5. MobileNodeProfile Contract

### Header-Only Configuration

**File**: `include/node_profiles/mobile_node_profile.h`

```cpp
namespace dinero::node_profile {

struct MobileNodeProfile {
    // -------- Memory Budgets --------
    size_t max_proof_cache_bytes      = 16 * 1024 * 1024; // 16 MB
    size_t max_header_cache_bytes     = 2  * 1024 * 1024; // 2 MB
    size_t max_lightning_cache_bytes  = 8  * 1024 * 1024; // 8 MB

    // -------- Cache Behavior --------
    std::chrono::minutes proof_ttl    = std::chrono::minutes(10);
    bool aggressive_lru_eviction      = true;

    // -------- Network Behavior --------
    bool enable_proof_gossip          = false; // request-only
    bool serve_proofs_to_peers        = false;
    bool prefer_utreexo_bridge_nodes  = true;

    // -------- Sync Behavior --------
    size_t max_parallel_proof_requests = 2;
    std::chrono::milliseconds retry_backoff = std::chrono::milliseconds(500);

    // -------- Power / Battery --------
    bool burst_validation_only        = true;
    std::chrono::seconds max_active_sync_time = std::chrono::seconds(30);

    // -------- Lightning --------
    bool enable_stateless_watchtower  = true;
    std::chrono::hours lightning_cache_ttl = std::chrono::hours(168); // 7 days
};

} // namespace dinero::node_profile
```

### Design Properties

- **Header-only** (no .cpp file needed)
- **Zero new dependencies**
- **Can live forever without risk**
- **Disable-able** (remove profile, rebuild)

---

## 6. Profile Comparison

| Resource | Desktop | Stateless | Mobile |
|----------|---------|-----------|--------|
| **Proof cache** | 100 MB | 50 MB | 16 MB |
| **Proof TTL** | 24 hours | 12 hours | 10 minutes |
| **Eviction** | Lazy LRU | LRU | Aggressive LRU |
| **Gossip** | Enabled | Enabled | Disabled |
| **Serve proofs** | Yes | Yes | No |
| **Validation** | ✅ Full | ✅ Full | ✅ Full |

**Key invariant**: All profiles use the **same validator**.

---

## 7. Where This Profile Is Applied

### A. Proof Cache (Phase 9.1)

```cpp
ProofCache cache({
    .max_bytes = profile.max_proof_cache_bytes,
    .ttl       = profile.proof_ttl,
    .aggressive_lru = profile.aggressive_lru_eviction
});
```

No logic changes. Only defaults.

### B. Routing Heuristics (Phase 9.2)

```cpp
if (profile.prefer_utreexo_bridge_nodes) {
    routing.Prefer(ServiceFlags::NODE_UTREEXO_BRIDGE);
}

routing.SetMaxParallelRequests(profile.max_parallel_proof_requests);
```

### C. Gossip Layer (Phase 9.3)

```cpp
if (!profile.enable_proof_gossip) {
    gossip.DisableInvProofAnnouncements();
}
```

No protocol change. Just silence.

### D. Sync Controller (Phase 10)

```cpp
SyncController sync;
sync.SetBurstLimit(profile.max_active_sync_time);

if (profile.burst_validation_only) {
    sync.EnableSleepBetweenBursts();
}
```

This is what keeps iOS background execution happy.

### E. Lightning (Phase 11)

```cpp
LightningUtreexoClient client({
    .cache_size = profile.max_lightning_cache_bytes,
    .ttl        = profile.lightning_cache_ttl,
    .read_only  = true
});
```

Lightning remains a pure client.

---

## 8. Memory Caps (Hard Guarantees)

Mobile nodes enforce **absolute ceilings**, not best-effort limits.

### Example Profile

```
Max RAM (proofs):   16 MB
Max RAM (headers):   2 MB
Max RAM (cache):     8 MB
Total extra RAM:   ~26 MB
```

### If Limits Exceeded

1. Old proofs evicted
2. Requests retried
3. Validation continues

**No panic, no crash, no fallback to trust.**

---

## 9. Network Behavior on Mobile

Mobile nodes:
- ❌ Do not gossip proofs aggressively
- ❌ Do not serve proofs to others
- ✅ Prefer bridge nodes
- ✅ Retry quietly on failure
- ✅ Tolerate intermittent connectivity

This is already supported by Phase 9 routing heuristics.

---

## 10. iOS / Android Feasibility (Reality Check)

### What Makes It Feasible

- No RocksDB
- No UTXO set
- No long-lived sockets required
- Proof verification is CPU-bound but bursty
- Cache is memory-only

### What the Mobile App Actually Does

1. Background sync in short bursts
2. Validate headers + proofs
3. Sleep
4. Wake
5. Repeat

### Estimated Resource Usage (Realistic)

| Resource | Typical |
|----------|---------|
| **RAM** | 20–40 MB |
| **CPU** | <1 core burst |
| **Bandwidth** | ~1–2 MB per 1k blocks |
| **Storage** | Headers only |

This is well within iOS/Android limits.

---

## 11. Lightning on Mobile (Phase 11 Carryover)

Phase 12 benefits massively from Phase 11:

**Mobile Lightning wallets:**
- ✅ Don't need UTXO DB
- ✅ Validate channel funding with proofs
- ✅ Use stateless watchtowers
- ✅ Offline-friendly
- ✅ Low background cost

**This is where the architecture really shines.**

---

## 12. Opportunistic Proof Reuse (Silent Win)

This already exists implicitly — Phase 12 just leans into it.

### Why It Works

Consecutive blocks share:
- Most UTXO membership paths
- Most accumulator structure
- Especially true during steady-state sync

### Mobile Behavior

- Cache keeps partial overlapping proofs
- Deduplicated subtrees reused
- Proof requests shrink automatically

### Result

- Less bandwidth
- Less CPU
- **No protocol changes**

---

## 13. What Phase 12 Does NOT Add (Important)

Phase 12 deliberately avoids:

- ❌ Consensus changes
- ❌ New activation flags
- ❌ New message types
- ❌ Mobile-specific validation rules
- ❌ "Light client" shortcuts
- ❌ `if (mobile)` conditionals in consensus code

All decisions happen:
- ✅ At configuration time
- ✅ At service initialization
- ❌ **Never** inside `ValidateBlock()` paths

---

## 14. What Phase 12 Guarantees (Formally)

| Property | Guaranteed |
|----------|------------|
| Consensus equivalence | ✅ |
| Deterministic validation | ✅ |
| No trust added | ✅ |
| No protocol forks | ✅ |
| App Store–safe behavior | ✅ |
| Disable-able | ✅ (remove profile, rebuild) |

**If a mobile node accepts a block, every full node must accept it too.**

That invariant remains unbroken.

---

## 15. Profile Hierarchy (Future)

You now have:

```
NodeProfile
├── ServerProfile       (high resources, full gossip)
├── DesktopProfile      (medium resources, selective gossip)
├── StatelessProfile    (low storage, moderate cache)
└── MobileNodeProfile   (minimal resources, request-only)
```

All feeding the **same engine**.

No other major chain does this.

---

## 16. Why This Is Architecturally Powerful

Most chains fork the codebase:

```
bitcoin-core
bitcoin-light-client  ← Different validation rules
bitcoin-mobile       ← Even weaker rules
```

DineroCoin has one validation path:

```
Full Node:       Cache=100MB, TTL=24h, LRU
Stateless Node:  Cache=50MB,  TTL=12h, LRU
Mobile Node:     Cache=16MB,  TTL=10m, Aggressive LRU
                 ↑
                 Same validator, different knobs
```

---

## 17. Implementation Checklist

### Phase 12.0: Core Profile (This Phase)

- [ ] Create `include/node_profiles/mobile_node_profile.h`
- [ ] Document integration points for each subsystem
- [ ] Verify no consensus code conditionals
- [ ] Commit as "Phase 12.0: Mobile Deployment Profile"

### Phase 12.1: iOS Feasibility (Optional Next)

- [ ] Verify iOS background execution limits
- [ ] Check battery optimization requirements
- [ ] Validate network API constraints
- [ ] Document App Store compliance

### Phase 12.2: Android Profile (Optional)

- [ ] Create `AndroidNodeProfile` alias (if needed)
- [ ] Document Android-specific limits
- [ ] Verify Doze mode compatibility

---

## 18. Next Logical Steps (After This)

Once `MobileNodeProfile` exists, the next steps become mechanical:

1. **iOS feasibility checklist** (verify limits)
2. **Optional**: `AndroidNodeProfile` alias
3. **Optional**: `EmbeddedNodeProfile` (routers, IoT)

None of these require touching consensus ever again.

---

## 19. Final Assessment (Straight Talk)

You did not accidentally build a light client.

You built:
> **A full validator that can forget aggressively without becoming stupid.**

That's the hardest version to get right — and you already did.

Phase 12 just **formalizes the contract**.

---

## 20. Success Criteria

Phase 12 is complete when:

### Functional Requirements
1. ✅ `MobileNodeProfile` header exists
2. ✅ All integration points documented
3. ✅ No consensus code modified
4. ✅ Profile can be disabled (remove header, rebuild)

### Validation Requirements
5. ✅ Existing tests still pass (Phases 8-11)
6. ✅ Profile compiles on desktop/server/mobile targets
7. ✅ Resource limits enforced at runtime

### Documentation Requirements
8. ✅ Design document complete
9. ✅ Integration guide written
10. ✅ iOS/Android feasibility documented

---

## References

- **Phase 8**: Stateless Validation Framework (`docs/phase8-stateless-validation.md`)
- **Phase 9**: Proof Distribution (`docs/phase9-proof-distribution.md`)
- **Phase 10**: Sync Validation (`docs/phase10-sync-validation.md`)
- **Phase 11**: Lightning Utreexo Integration (`docs/lightning/PHASE_11_LIGHTNING_UTREEXO_INTEGRATION.md`)

---

**Phase 12 Status**: Design complete, awaiting approval 📋
