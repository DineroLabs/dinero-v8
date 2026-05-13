# Phase 12: Profile Integration Guide

This document shows **exactly where** `MobileNodeProfile` parameters are applied to existing Phase 8–11 systems.

**Critical Rule**: No conditionals inside consensus code. All decisions at configuration/initialization time.

---

## Integration Points

### 1. Phase 9.1: Proof Cache

**File**: `src/consensus/proof_cache.cpp`

**Integration**:

```cpp
#include "node_profiles/mobile_node_profile.h"

// At initialization (e.g., in main() or node startup)
dinero::node_profile::MobileNodeProfile profile;

// Configure proof cache with profile defaults
ProofCache cache;
cache.SetMaxBytes(profile.max_proof_cache_bytes);        // 16 MB
cache.SetTTL(profile.proof_ttl);                         // 10 minutes
cache.SetAggressiveLRU(profile.aggressive_lru_eviction); // true
```

**What This Does**:
- Limits cache size to 16 MB (vs 100 MB desktop default)
- Evicts proofs after 10 minutes (vs 24 hours desktop default)
- Enables aggressive eviction when approaching limit

**Consensus Impact**: ❌ None (cache is optimization only)

---

### 2. Phase 9.2: Proof Router

**File**: `src/consensus/proof_router.cpp`

**Integration**:

```cpp
#include "node_profiles/mobile_node_profile.h"

dinero::node_profile::MobileNodeProfile profile;

// Configure routing heuristics
ProofRouter router;

if (profile.prefer_utreexo_bridge_nodes) {
    router.PreferServiceFlag(ServiceFlags::NODE_UTREEXO_BRIDGE);
}

router.SetMaxParallelRequests(profile.max_parallel_proof_requests); // 2
router.SetRetryBackoff(profile.retry_backoff);                      // 500ms

if (!profile.serve_proofs_to_peers) {
    router.DisableServing();
}
```

**What This Does**:
- Prioritizes bridge nodes (designed to serve proofs efficiently)
- Limits concurrent proof requests to 2 (vs 8-16 desktop default)
- Sets retry backoff to 500ms for intermittent connectivity
- Disables serving proofs to other peers (consume only, don't serve)

**Consensus Impact**: ❌ None (network behavior only)

---

### 3. Phase 9.3: Proof Gossip

**File**: `src/consensus/proof_gossip.cpp`

**Integration**:

```cpp
#include "node_profiles/mobile_node_profile.h"

dinero::node_profile::MobileNodeProfile profile;

// Configure gossip layer
ProofGossip gossip;

if (!profile.enable_proof_gossip) {
    gossip.DisableInvProofAnnouncements();  // No gossip, request-only
}
```

**What This Does**:
- Disables `INV_PROOF` message announcements
- Mobile node becomes request-only (doesn't advertise proofs)
- Reduces network traffic and battery usage

**Consensus Impact**: ❌ None (gossip is optional)

---

### 4. Phase 10: Sync Controller

**File**: `src/consensus/sync_simulator.cpp` (or equivalent sync logic)

**Integration**:

```cpp
#include "node_profiles/mobile_node_profile.h"

dinero::node_profile::MobileNodeProfile profile;

// Configure sync behavior
SyncController sync;

if (profile.burst_validation_only) {
    sync.EnableBurstMode();
    sync.SetBurstLimit(profile.max_active_sync_time);  // 30 seconds
}
```

**What This Does**:
- Enables burst validation mode
- Syncs for max 30 seconds, then sleeps
- iOS background execution limit is ~30 seconds
- Keeps app from being terminated by OS

**Burst Cycle**:
1. Wake up
2. Validate headers + proofs (30 seconds max)
3. Sleep
4. Repeat

**Consensus Impact**: ❌ None (pacing only, validation unchanged)

---

### 5. Phase 11: Lightning Utreexo Client

**File**: `src/lightning/utreexo/lightning_utreexo_client.cpp`

**Integration**:

```cpp
#include "node_profiles/mobile_node_profile.h"

dinero::node_profile::MobileNodeProfile profile;

// Configure Lightning cache
LightningProofCache cache;
cache.SetMaxBytes(profile.max_lightning_cache_bytes);  // 8 MB
cache.SetTTL(profile.lightning_cache_ttl);             // 7 days

// Enable stateless watchtower
if (profile.enable_stateless_watchtower) {
    WatchtowerProofMonitor watchtower(utreexo_client);
    watchtower.WatchChannel(channel_id, commitment, secrets);
}
```

**What This Does**:
- Limits Lightning cache to 8 MB
- Keeps Lightning proofs for 7 days (channel-lifetime scoped)
- Enables stateless watchtower (proofs + headers only, no UTXO DB)

**Why This Matters**:
- Mobile Lightning wallets don't need UTXO DB
- Validate channel funding with proofs
- Monitor breaches statelessly
- Low background cost

**Consensus Impact**: ❌ None (Lightning is client-only, Phase 11)

---

## Example: Mobile Node Initialization

**Pseudo-code** showing complete integration:

```cpp
#include "node_profiles/mobile_node_profile.h"

int main() {
    // 1. Select profile
    dinero::node_profile::MobileNodeProfile profile;

    // 2. Initialize Phase 9.1 (Proof Cache)
    ProofCache proof_cache;
    proof_cache.SetMaxBytes(profile.max_proof_cache_bytes);
    proof_cache.SetTTL(profile.proof_ttl);
    proof_cache.SetAggressiveLRU(profile.aggressive_lru_eviction);

    // 3. Initialize Phase 9.2 (Proof Router)
    ProofRouter router;
    if (profile.prefer_utreexo_bridge_nodes) {
        router.PreferServiceFlag(ServiceFlags::NODE_UTREEXO_BRIDGE);
    }
    router.SetMaxParallelRequests(profile.max_parallel_proof_requests);
    router.SetRetryBackoff(profile.retry_backoff);
    if (!profile.serve_proofs_to_peers) {
        router.DisableServing();
    }

    // 4. Initialize Phase 9.3 (Proof Gossip)
    ProofGossip gossip;
    if (!profile.enable_proof_gossip) {
        gossip.DisableInvProofAnnouncements();
    }

    // 5. Initialize Phase 10 (Sync Controller)
    SyncController sync;
    if (profile.burst_validation_only) {
        sync.EnableBurstMode();
        sync.SetBurstLimit(profile.max_active_sync_time);
    }

    // 6. Initialize Phase 11 (Lightning)
    LightningProofCache lightning_cache;
    lightning_cache.SetMaxBytes(profile.max_lightning_cache_bytes);
    lightning_cache.SetTTL(profile.lightning_cache_ttl);

    if (profile.enable_stateless_watchtower) {
        WatchtowerProofMonitor watchtower(/* ... */);
        // Enable stateless watchtower
    }

    // 7. Start node
    // ... validation logic (UNCHANGED from desktop) ...

    return 0;
}
```

**Key Observations**:
- ✅ All configuration happens before validation starts
- ✅ No conditionals in consensus code
- ✅ Same validator used across all profiles
- ✅ Disable profile = remove header, rebuild

---

## Verification: No Consensus Changes

**Critical Test**:

```bash
# Desktop profile validation
./dinerod --profile=desktop validate_block <block_hash>

# Mobile profile validation
./dinerod --profile=mobile validate_block <block_hash>

# Stateless profile validation
./dinerod --profile=stateless validate_block <block_hash>
```

**Expected Result**: All three produce **identical validation outcomes**.

If they differ → **BUG** (profile leaked into consensus)

---

## Profile Switching (Runtime)

**Option 1**: Compile-time selection

```cpp
#ifdef MOBILE_BUILD
    dinero::node_profile::MobileNodeProfile profile;
#else
    dinero::node_profile::DesktopNodeProfile profile;
#endif
```

**Option 2**: Command-line flag

```cpp
std::string profile_type = GetArg("-profile", "desktop");

if (profile_type == "mobile") {
    dinero::node_profile::MobileNodeProfile profile;
    // ...
} else if (profile_type == "stateless") {
    dinero::node_profile::StatelessNodeProfile profile;
    // ...
} else {
    dinero::node_profile::DesktopNodeProfile profile;
    // ...
}
```

**Option 3**: Config file

```ini
[node]
profile = mobile
```

---

## Memory Budget Enforcement

Mobile nodes must enforce **hard limits**, not best-effort:

```cpp
class ProofCache {
    void Add(const uint256& key, const BlockUtreexoData& proof) {
        // Check size before adding
        if (CurrentSize() + proof.Size() > max_bytes_) {
            // Evict until we have space
            EvictLRU(proof.Size());
        }

        // Add proof
        cache_[key] = proof;

        // Verify we didn't exceed (assert in debug builds)
        assert(CurrentSize() <= max_bytes_);
    }
};
```

**If limits exceeded**:
1. Evict old proofs (LRU)
2. Retry request
3. Continue validation

**Never**:
- ❌ Panic
- ❌ Crash
- ❌ Fall back to trust

---

## Testing Strategy

### Unit Tests

**Test**: Profile parameters are applied correctly

```cpp
TEST(MobileNodeProfile, CacheSizing) {
    MobileNodeProfile profile;
    ProofCache cache;
    cache.SetMaxBytes(profile.max_proof_cache_bytes);

    EXPECT_EQ(cache.GetMaxBytes(), 16 * 1024 * 1024);  // 16 MB
}

TEST(MobileNodeProfile, TTL) {
    MobileNodeProfile profile;
    ProofCache cache;
    cache.SetTTL(profile.proof_ttl);

    EXPECT_EQ(cache.GetTTL(), std::chrono::minutes(10));
}
```

### Integration Tests

**Test**: Mobile profile doesn't change validation

```cpp
TEST(MobileNodeProfile, ConsensusEquivalence) {
    // Desktop profile
    DesktopNodeProfile desktop;
    auto result_desktop = ValidateBlock(block, desktop);

    // Mobile profile
    MobileNodeProfile mobile;
    auto result_mobile = ValidateBlock(block, mobile);

    // MUST be identical
    EXPECT_EQ(result_desktop.valid, result_mobile.valid);
    EXPECT_EQ(result_desktop.error, result_mobile.error);
}
```

### Resource Tests

**Test**: Memory limits are enforced

```cpp
TEST(MobileNodeProfile, MemoryLimits) {
    MobileNodeProfile profile;
    ProofCache cache(profile.max_proof_cache_bytes);

    // Add proofs until limit
    while (cache.CurrentSize() < profile.max_proof_cache_bytes) {
        cache.Add(RandomHash(), RandomProof());
    }

    // Verify hard limit
    EXPECT_LE(cache.CurrentSize(), profile.max_proof_cache_bytes);

    // Add more proofs (should trigger eviction)
    for (int i = 0; i < 100; i++) {
        cache.Add(RandomHash(), RandomProof());
    }

    // Still under limit
    EXPECT_LE(cache.CurrentSize(), profile.max_proof_cache_bytes);
}
```

---

## Future Profiles

### EmbeddedNodeProfile (Routers, IoT)

```cpp
struct EmbeddedNodeProfile {
    size_t max_proof_cache_bytes     = 4  * 1024 * 1024;  // 4 MB
    size_t max_header_cache_bytes    = 1  * 1024 * 1024;  // 1 MB
    std::chrono::minutes proof_ttl   = std::chrono::minutes(5);
    // ... even more constrained
};
```

### ServerProfile (Data centers)

```cpp
struct ServerProfile {
    size_t max_proof_cache_bytes     = 500 * 1024 * 1024;  // 500 MB
    std::chrono::hours proof_ttl     = std::chrono::hours(48);
    bool aggressive_lru_eviction     = false;
    bool serve_proofs_to_peers       = true;  // Help network
    // ... high resources
};
```

---

## Summary

Phase 12 profile integration is:
- ✅ **Clean**: All configuration at initialization
- ✅ **Safe**: No consensus code changes
- ✅ **Reversible**: Disable by removing header
- ✅ **Testable**: Validation equivalence across profiles
- ✅ **Maintainable**: Header-only, zero dependencies

**If you can change validation by changing a profile, you have a bug.**

Profiles tune **resource usage**, never **validation rules**.
