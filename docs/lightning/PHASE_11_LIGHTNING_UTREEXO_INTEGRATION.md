# Phase 11: Lightning Utreexo Integration

**Status**: Design Phase
**Depends On**: Phase 8 (Stateless Validation), Phase 9 (Proof Distribution)
**Date**: 2026-01-10

---

## One-Sentence Summary

**Phase 11 teaches Lightning how to consume Utreexo proofs and Phase 9 infrastructure without ever influencing consensus.**

---

## Critical Confirmation

✅ Lightning is **NOT** Phase 8
✅ Lightning is **NOT** Phase 9
✅ Lightning is a **CLIENT** of both

**Rule**: Lightning may consume proofs — it may never define them.

---

## 1. Scope & Non-Goals (MANDATORY)

### ✅ In Scope

**Phase 11 implements**:
- Utreexo proof consumption
- Proof caching for Lightning (separate cache, Lightning-specific TTLs)
- Stateless watchtower validation
- Channel/HTLC validation using proofs
- Proof fetching from Phase 9 infrastructure (cache → router → gossip)
- Proof verification against Utreexo roots
- Graceful degradation when proofs unavailable

**Deployment modes enabled**:
- Full node + Lightning
- Stateless Lightning node
- Stateless watchtower
- Mobile Lightning client (future)

### ❌ Out of Scope (FORBIDDEN)

**Phase 11 MUST NOT**:
- ❌ Modify consensus rules
- ❌ Change script semantics
- ❌ Add covenant logic
- ❌ Define proof formats
- ❌ Touch Phase 8 validation logic
- ❌ Modify Phase 9 protocol
- ❌ Add new gRPC services
- ❌ Require new Homebrew dependencies
- ❌ Break `DINERO_RELEASE=ON` build

**Hard boundary**: Phase 11 is a client of Phases 8–9, never a modifier.

---

## 2. Architectural Positioning

### System Architecture

```
            ┌─────────────────────────────┐
            │       dinerod               │
            │  (consensus layer)          │
            │                             │
            │  - Block validation         │
            │  - Utreexo enforcement      │
            │  - Proof distribution       │
            └──────────▲──────────────────┘
                       │
                       │ gRPC / Socket (READ-ONLY)
                       │
            ┌──────────┴──────────────────┐
            │     lightningd              │
            │  (Phase 11 logic)           │
            │                             │
            │  - Proof consumption        │
            │  - Channel validation       │
            │  - Watchtower monitoring    │
            └──────────▲──────────────────┘
                       │
                       │ Lightning P2P
                       │
              Lightning Network
```

### Key Invariants

1. **Read-only interaction**: `lightningd` cannot change chain behavior — only react to it
2. **No shared state**: Lightning and consensus have zero mutable shared state
3. **Separate processes**: Lightning runs as `lightningd` daemon, not embedded in `dinerod`
4. **IPC boundary**: Communication via gRPC (dev) or sockets (release), never direct calls

### Build Modes

**Dev Mode** (default):
```
lightningd → gRPC client → dinerod
             ↓
             System protobuf/grpc++ (Homebrew OK)
```

**Release Mode** (`DINERO_RELEASE=ON`):
```
lightningd → Socket transport → dinerod
             ↓
             Vendored protobuf (static)
             Raw TCP sockets
             ZERO Homebrew dependencies
```

**Critical constraint**: `otool -L build/lightningd | grep homebrew` → **0 results**

---

## 3. Utreexo Integration Model

### Proof Consumption Flow

**Lightning consumes already-validated outputs**:

```
1. dinerod validates block (Phase 8)
   ↓
2. dinerod distributes proof (Phase 9)
   ↓
3. lightningd fetches proof (Phase 11)
   ↓
4. lightningd verifies proof against root
   ↓
5. lightningd uses proof for channel/HTLC validation
```

### Required Data from dinerod

Lightning queries dinerod for:
- Block headers + Utreexo roots
- Proof blobs (from Phase 9 cache/gossip)
- Confirmation depth
- Current chain height

### What Lightning NEVER Does

Lightning never:
- ❌ Builds proofs
- ❌ Trusts proofs without verification
- ❌ Requests special consensus behavior
- ❌ Modifies validation rules
- ❌ Bypasses Phase 8 stateless validation

---

## 4. Proof Verification in Lightning

### Verification Steps

When Lightning receives a proof:

1. **Fetch proof** (multi-tier):
   - Try local cache (fast path)
   - Try Phase 9.1 proof cache (via gRPC/socket)
   - Try Phase 9.2 proof router (peer selection)
   - Try Phase 9.3 proof gossip (network broadcast)

2. **Verify proof** (using Phase 8 logic):
   - ✅ Membership proof valid
   - ✅ Root matches block header
   - ✅ Script validity (Phase 8 sealed rules)
   - ✅ UTXO amount matches expected

3. **Cache result** (Lightning-specific cache):
   - TTL-bound (different from Phase 9.1)
   - Re-verified on use (never trust cache)
   - Evicted aggressively on mismatch

4. **Use proof**:
   - Channel funding verification
   - HTLC settlement validation
   - Watchtower breach detection

### Rejection Semantics

**Lightning rejects channels/HTLCs — NOT blocks**:
- Invalid proof → Reject channel opening
- Invalid proof → Reject HTLC settlement
- Invalid proof → Watchtower alerts (no chain action)

**Lightning failure ≠ consensus failure**

---

## 5. Stateless Watchtower Design (Key Phase 11 Feature)

### Watchtower Properties

**Stateless watchtower has**:
- ✅ Block headers
- ✅ Utreexo roots
- ✅ Proofs (on-demand from Phase 9)
- ✅ Channel state

**Stateless watchtower does NOT have**:
- ❌ UTXO database
- ❌ Mempool
- ❌ Full block storage
- ❌ chainstate DB

### Watchtower Validation

**Watchtower validates** (using Phase 8 + Phase 9):
1. Breach transactions (detect revoked commitments)
2. HTLC claims (verify preimage/timeout)
3. Timelock enforcement (CLTV/CSV)

**Watchtower uses**:
- Phase 8: Stateless validation framework
- Phase 9: Proof distribution (cache, router, gossip)
- Phase 11: Lightning-specific proof consumption

### Breach Detection Flow

```
1. Monitor blockchain for commitment transactions
   ↓
2. Fetch proof for commitment UTXO (Phase 9)
   ↓
3. Verify commitment is revoked (Lightning logic)
   ↓
4. Construct breach remedy transaction
   ↓
5. Broadcast breach remedy (claim all funds)
```

**This is one of the big wins of this architecture** 🎯

---

## 6. Proof Cache (Lightning-Specific)

### Why Separate Cache?

**Lightning cache is separate from Phase 9.1 core cache**:

| Property | Phase 9.1 Cache | Lightning Cache |
|----------|-----------------|-----------------|
| Scope | Global, network-wide | Channel-specific |
| TTL | Short (minutes-hours) | Long (channel lifetime + 1 week) |
| Eviction | LRU (space-limited) | Channel-based (lifecycle) |
| Trust | Cache ≠ trust | Cache ≠ trust (same) |

### Lightning Cache Rules

1. **Cache ≠ trust**: Always re-verify proofs before use
2. **Evict aggressively**: On proof mismatch, evict immediately
3. **Channel-bound TTL**: Proofs for closed channels evicted after 1 week
4. **Space-limited**: Max 1000 proofs (LRU eviction if exceeded)

### Cache Interface

```cpp
namespace lightning {

class LightningProofCache {
public:
    // Cache proof for channel
    void CacheChannelProof(
        const std::string& channel_id,
        const uint256& utxo_hash,
        const consensus::BlockUtreexoData& proof
    );

    // Get cached proof (returns nullopt if expired/missing)
    std::optional<consensus::BlockUtreexoData> GetProof(
        const uint256& utxo_hash
    );

    // Evict all proofs for closed channel
    void EvictChannelProofs(const std::string& channel_id);

    // Clear entire cache (on reorg)
    void Clear();

private:
    // channel_id → set of UTXO hashes
    std::unordered_map<std::string, std::unordered_set<uint256>> channel_proofs_;

    // UTXO hash → (proof, expiry_time)
    std::unordered_map<uint256, std::pair<consensus::BlockUtreexoData, uint64_t>> proof_cache_;

    mutable std::mutex mutex_;
};

} // namespace lightning
```

---

## 7. Failure Model (VERY IMPORTANT)

### Failure Handling

| Failure | Lightning Action | Chain Impact |
|---------|------------------|--------------|
| **Proof unavailable** | Retry / delay channel operation | ❌ None |
| **Proof invalid** | Reject channel/HTLC | ❌ None |
| **Cache poisoned** | Evict + penalize peer | ❌ None |
| **Lightning crash** | Restart, re-sync channels | ❌ None |
| **Chain reorg** | Re-validate via proofs | ❌ None |

### Critical Property

**Lightning failure is NEVER consensus failure**:
- ✅ Lightning can crash → blockchain unaffected
- ✅ Lightning can have bad proofs → consensus unchanged
- ✅ Lightning can reject all channels → chain continues

**Failure isolation**: Lightning layer failures do NOT propagate to consensus layer.

### Graceful Degradation

When proofs are unavailable:
1. **Defer channel operations** (safe delay)
2. **Request from multiple peers** (Phase 9.2 router)
3. **Fall back to on-chain verification** (if available)
4. **Alert user, never panic** (UX degradation, not failure)

**Proof unavailability ≠ Lightning failure** (tolerate gracefully)

---

## 8. Security Boundaries (HARD RULES)

### Compilation Boundaries

1. **Lightning code never links against consensus internals**:
   - ❌ No `#include "consensus/block_validation.h"` in Lightning
   - ❌ No shared symbols between `dinerod` and `lightningd`
   - ✅ Only IPC communication (gRPC or sockets)

2. **gRPC is read-only**:
   - ✅ Lightning can query blockchain state
   - ❌ Lightning cannot modify blockchain state
   - ✅ All mutations rejected at gRPC server

3. **No shared mutable state**:
   - ❌ No global variables
   - ❌ No shared memory
   - ✅ Separate processes, separate address spaces

4. **No "fast paths" around validation**:
   - ❌ No cached validation bypass
   - ❌ No "trusted peer" exemptions
   - ✅ Every proof verified, every time

### Runtime Boundaries

1. **Process isolation**:
   - `dinerod` runs as PID 1234
   - `lightningd` runs as PID 5678
   - No direct memory access

2. **Network isolation**:
   - Core P2P: Port 8333 (Bitcoin-style)
   - Lightning P2P: Port 9735 (BOLT spec)
   - No mixed protocols

3. **Data isolation**:
   - Core data: `~/.dinero/blocks`, `~/.dinero/chainstate`
   - Lightning data: `~/.dinero/lightning/channels`
   - No shared files

### Attack Resistance

**Phase 11 resists**:
- ✅ Proof withholding → Retry with different peers (Phase 9.2)
- ✅ Invalid proofs → Cryptographic verification (Phase 8)
- ✅ DoS attacks → Rate limiting + timeouts (Phase 10 proven)
- ✅ Eclipse attacks → Multiple proof sources (Phase 9.3 gossip)

**Phase 11 does NOT need to resist**:
- Consensus attacks (Phase 8 responsibility)
- Proof distribution attacks (Phase 9 responsibility)

---

## 9. Deployment Modes Enabled

### Mode 1: Full Node + Lightning

```
dinerod (full UTXO set) + lightningd (proof-aware)
                          ↓
                          Uses proofs for redundancy check
                          Falls back to UTXO DB if proof missing
```

**Use case**: Traditional full node with Lightning channels

### Mode 2: Stateless Node + Lightning

```
dinerod (stateless, no UTXO DB) + lightningd (proof-based)
                                  ↓
                                  MUST use proofs for all validation
                                  No UTXO fallback available
```

**Use case**: Resource-constrained nodes (e.g., Raspberry Pi)

### Mode 3: Watchtower Only

```
lightningd (watchtower mode, no wallet, no channels)
           ↓
           Monitors other nodes' channels
           Uses only proofs + headers
           No UTXO DB, no mempool, no full blocks
```

**Use case**: Dedicated breach monitoring service

### Mode 4: Mobile Lightning (Future)

```
lightningd (mobile, no dinerod)
           ↓
           Queries proofs from remote full node
           Minimal storage (headers + proofs only)
           Battery-efficient
```

**Use case**: Mobile wallets, embedded devices

**All modes possible without touching consensus** ✅

---

## 10. Test Strategy

### Lightning-Specific Tests ONLY

**Phase 11 test files** (`tests/lightning/`):
- `test_lightning_utreexo_client.cpp` (T11.1–T11.5)
- `test_channel_proof_validator.cpp` (T11.6–T11.10)
- `test_htlc_proof_validator.cpp` (T11.11–T11.15)
- `test_watchtower_stateless.cpp` (T11.16–T11.20)

**Test scope**:
- ✅ Channel funding with proofs
- ✅ HTLC resolution with proofs
- ✅ Watchtower breach detection
- ✅ Reorg handling
- ✅ Cache eviction
- ✅ Graceful degradation

### Existing Tests MUST Remain Unchanged

**Phase 8 tests** (stateless validation):
- ❌ Do NOT modify
- ✅ Must continue passing
- Regression: Any Phase 8 test failure → Phase 11 rejected

**Phase 9 tests** (proof distribution):
- ❌ Do NOT modify
- ✅ Must continue passing
- Regression: Any Phase 9 test failure → Phase 11 rejected

**Phase 10 tests** (sync validation):
- ❌ Do NOT modify
- ✅ Must continue passing
- Regression: Any Phase 10 test failure → Phase 11 rejected

### Test Execution

```bash
# Phase 11 tests (new)
./build/test_lightning_utreexo_client
./build/test_channel_proof_validator
./build/test_htlc_proof_validator
./build/test_watchtower_stateless

# Regression check (existing tests must pass)
./build/test_utreexo_stateless_validation  # Phase 8
./build/test_proof_cache                   # Phase 9.1
./build/test_proof_router                  # Phase 9.2
./build/test_proof_gossip                  # Phase 9.3
./build/test_proof_compression             # Phase 9.4
./build/test_lightning_proof_client        # Phase 9.5
./build/test_sync_validation               # Phase 10
```

**Success criteria**: All new tests pass + zero regressions

---

## Components (Implementation Details)

### 11.1: Lightning Utreexo Client

**File**: `src/lightning/utreexo/lightning_utreexo_client.cpp`

**Purpose**: Read-only proof query client for Lightning

**Interface**:
```cpp
namespace lightning {

class LightningUtreexoClient {
public:
    // Construct with Phase 9.5 proof provider
    explicit LightningUtreexoClient(
        std::shared_ptr<consensus::ProofQueryInterface> proof_provider
    );

    // Query channel funding proof
    std::optional<consensus::BlockUtreexoData> GetChannelFundingProof(
        const uint256& funding_txid,
        uint32_t funding_vout,
        const uint256& expected_root
    );

    // Query HTLC proof
    std::optional<consensus::BlockUtreexoData> GetHTLCProof(
        const uint256& htlc_txid,
        uint32_t htlc_vout,
        const uint256& expected_root
    );

    // Prefetch proofs for channel
    size_t PrefetchChannelProofs(
        const std::string& channel_id,
        uint32_t block_height
    );

    // Statistics
    struct Stats {
        uint64_t channel_funding_queries = 0;
        uint64_t htlc_queries = 0;
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        uint64_t proof_unavailable = 0;
    };
    Stats GetStats() const;

private:
    std::shared_ptr<consensus::ProofQueryInterface> proof_provider_;
    LightningProofCache cache_;  // Lightning-specific cache
    Stats stats_;
    mutable std::mutex mutex_;
};

} // namespace lightning
```

**Key properties**:
- Wraps Phase 9.5 `ProofQueryInterface` (read-only)
- Maintains Lightning-specific proof cache
- Tracks statistics for monitoring

**Tests**: T11.1–T11.5

---

### 11.2: Channel Proof Validator

**File**: `src/lightning/utreexo/channel_proof_validator.cpp`

**Purpose**: Verify channel funding using Utreexo proofs

**Interface**:
```cpp
namespace lightning {

class ChannelProofValidator {
public:
    // Validation result
    struct ValidationResult {
        bool valid;
        std::string error;
        uint32_t confirmations;
    };

    // Validate funding proof
    ValidationResult ValidateFundingProof(
        const uint256& funding_txid,
        uint32_t funding_vout,
        uint64_t expected_amount,
        const consensus::BlockUtreexoData& proof,
        const uint256& utreexo_root
    );

    // Check minimum confirmations
    bool HasMinimumConfirmations(
        const uint256& funding_txid,
        uint32_t min_confirmations
    );

private:
    std::shared_ptr<LightningUtreexoClient> utreexo_client_;
};

} // namespace lightning
```

**Integration**: Called by `ChannelManager::OpenChannel()` before accepting funding

**Tests**: T11.6–T11.10

---

### 11.3: HTLC Proof Validator

**File**: `src/lightning/utreexo/htlc_proof_validator.cpp`

**Purpose**: Verify HTLC settlements using Utreexo proofs

**Interface**:
```cpp
namespace lightning {

class HTLCProofValidator {
public:
    // Validation result
    struct HTLCValidationResult {
        bool valid;
        std::string error;
        bool timeout_eligible;
        bool preimage_match;
    };

    // Validate HTLC settlement
    HTLCValidationResult ValidateHTLCSettlement(
        const uint256& htlc_txid,
        uint32_t htlc_vout,
        const std::vector<uint8_t>& preimage,
        const consensus::BlockUtreexoData& proof,
        const uint256& utreexo_root
    );

    // Check timeout eligibility
    bool IsTimeoutEligible(
        const std::vector<uint8_t>& htlc_script,
        uint32_t current_height,
        uint64_t current_time
    );

private:
    std::shared_ptr<LightningUtreexoClient> utreexo_client_;
};

} // namespace lightning
```

**Integration**: Called by `HTLCManager::SettleHTLC()` before claiming payment

**Tests**: T11.11–T11.15

---

### 11.4: Watchtower Proof Monitor

**File**: `src/lightning/utreexo/watchtower_proof_monitor.cpp`

**Purpose**: Stateless watchtower using Utreexo proofs

**Interface**:
```cpp
namespace lightning {

class WatchtowerProofMonitor {
public:
    // Breach detection result
    struct BreachDetectionResult {
        bool breach_detected;
        uint256 breach_txid;
        uint64_t commitment_number;
        std::vector<uint8_t> remedy_tx;
    };

    // Monitor channel for breaches
    BreachDetectionResult MonitorChannel(
        const std::string& channel_id,
        const std::vector<uint8_t>& latest_commitment,
        const std::vector<std::vector<uint8_t>>& revocation_secrets
    );

    // Verify commitment proof
    bool VerifyCommitmentProof(
        const std::vector<uint8_t>& commitment_tx,
        const consensus::BlockUtreexoData& proof,
        const uint256& utreexo_root
    );

    // Construct breach remedy
    std::optional<std::vector<uint8_t>> ConstructBreachRemedy(
        const uint256& breach_txid,
        const std::vector<uint8_t>& revocation_secret,
        const consensus::BlockUtreexoData& proof
    );

private:
    std::shared_ptr<LightningUtreexoClient> utreexo_client_;
    std::unordered_map<std::string, std::vector<uint8_t>> watched_channels_;
    mutable std::mutex mutex_;
};

} // namespace lightning
```

**Integration**: Called by `WatchtowerClient` for stateless monitoring

**Tests**: T11.16–T11.20

---

## Success Criteria

**Phase 11 is complete when**:

### Functional Requirements
1. ✅ All 20 tests pass (T11.1–T11.20)
2. ✅ Lightning queries proofs via Phase 9.5 interface
3. ✅ Channel funding verified with proofs
4. ✅ HTLC settlements validated with proofs
5. ✅ Stateless watchtowers operational

### Build Requirements
6. ✅ Dev mode builds successfully (`cmake .. && make lightningd`)
7. ✅ Release mode builds successfully (`cmake -DDINERO_RELEASE=ON .. && make lightningd`)
8. ✅ Zero new Homebrew dependencies (`otool -L lightningd | grep homebrew` → 0 results)

### Regression Requirements
9. ✅ Phase 8 tests continue passing (no stateless validation regressions)
10. ✅ Phase 9 tests continue passing (no proof distribution regressions)
11. ✅ Phase 10 tests continue passing (no sync validation regressions)

### Code Quality Requirements
12. ✅ No consensus code modified (`git diff src/consensus/` → empty)
13. ✅ No new gRPC services added
14. ✅ All code in `src/lightning/` directory
15. ✅ Documentation complete

---

## Implementation Roadmap

### Step 11.1: Lightning Utreexo Client (Week 1)
- Create `lightning_utreexo_client.h/cpp`
- Implement proof query methods
- Add Lightning-specific cache
- Write tests T11.1–T11.5
- **Exit criteria**: Tests pass, no Phase 9 regressions

### Step 11.2: Channel Proof Validator (Week 2)
- Create `channel_proof_validator.h/cpp`
- Implement funding proof verification
- Add confirmation checking
- Write tests T11.6–T11.10
- **Exit criteria**: Tests pass, channels validate with proofs

### Step 11.3: HTLC Proof Validator (Week 3)
- Create `htlc_proof_validator.h/cpp`
- Implement HTLC settlement validation
- Add timeout checking
- Write tests T11.11–T11.15
- **Exit criteria**: Tests pass, HTLCs settle with proofs

### Step 11.4: Watchtower Proof Monitor (Week 4)
- Create `watchtower_proof_monitor.h/cpp`
- Implement breach detection
- Add breach remedy construction
- Write tests T11.16–T11.20
- **Exit criteria**: Tests pass, watchtowers operate statelessly

### Step 11.5: Integration & Testing (Week 5)
- Integrate with `ChannelManager`
- Integrate with `HTLCManager`
- Integrate with `WatchtowerClient`
- Run full regression suite
- Verify release mode build
- **Exit criteria**: All tests pass, zero regressions

---

## References

- **Phase 8**: Stateless Validation Framework (`docs/phase8-stateless-validation.md`)
- **Phase 9**: Proof Distribution (`docs/phase9-proof-distribution.md`)
- **Phase 9.5**: Lightning Proof Client (`include/consensus/lightning_proof_client.h`)
- **Phase 10**: Sync Validation (`docs/phase10-sync-validation.md`)
- **BOLT Specs**: https://github.com/lightning/bolts
- **Utreexo Paper**: https://eprint.iacr.org/2019/611

---

## Approval Checklist

Before implementing Phase 11, confirm:

- [ ] Design reviewed and approved
- [ ] Scope boundaries clear (in-scope vs out-of-scope)
- [ ] Architectural positioning understood (Lightning = client)
- [ ] Build mode compatibility verified (dev + release)
- [ ] Security boundaries acknowledged (hard rules)
- [ ] Test strategy agreed upon (Lightning tests only, no regressions)
- [ ] Success criteria clear (functional + build + regression)

**Once approved, proceed with Step 11.1 implementation** ✅

---

**Phase 11 Status**: Design complete, awaiting approval 📋
