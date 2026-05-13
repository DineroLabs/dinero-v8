# Phase 13: Deployment & User Experience

**Status:** Design Specification
**Author:** DineroCoin Core Development
**Date:** 2026-01-10
**Depends On:** Phase 8 (Consensus), Phase 9 (Proof Network), Phase 10 (Sync), Phase 11 (Lightning), Phase 12 (Mobile Profile)

---

## Executive Summary

**What Phase 13 Is:**

Phase 13 is the **translation layer** between a mathematically sound validator and a human-operated product.

It defines:
- What the UI can **honestly** say about validation state
- How different deployment profiles **identify** themselves
- What failures must **never** be hidden from users
- What time/battery/resource **contracts** the software makes
- What "verified" **actually means** in Lightning operations

**What Phase 13 Is NOT:**

- ❌ New consensus logic (validation is complete)
- ❌ New cryptography (proofs are complete)
- ❌ Marketing copy (this is technical honesty)
- ❌ Optional best practices (these are **mandatory** rules)

**Core Principle:**

> **"The UI must never imply correctness the validator cannot prove."**

If the validator hasn't cryptographically verified a block, the UI cannot show "synced."
If proofs are missing, the UI cannot show "safe."
If validation is paused, the UI cannot show "complete."

This is the discipline that prevents DineroCoin from becoming "just another altcoin with dishonest claims."

---

## Foundation: What Phases 8-12 Guarantee

Before defining what the UI can say, we must understand what the **validator** can prove:

### Phase 8: Cryptographic Consensus
- ✅ Block headers are valid (PoW, merkle roots, timestamps)
- ✅ Utreexo accumulator state transitions are correct
- ✅ Spent outputs existed and were unspent at validation time
- ❌ **CANNOT** prove a block is valid without its proof
- ❌ **CANNOT** prove "the chain is safe" (only "this block passed validation")

### Phase 9: Proof Availability
- ✅ Proofs can be re-fetched from bridge nodes
- ✅ Proof cache eviction is safe (re-fetch works)
- ✅ Cached proofs are always re-verified (cache ≠ trust)
- ❌ **CANNOT** guarantee proofs are instantly available
- ❌ **CANNOT** validate without proofs (temporary unavailability is honest)

### Phase 10: Sync & Resumability
- ✅ Sync can resume after interruption
- ✅ Last validated height is persisted
- ✅ Validation progress is monotonic (never regresses)
- ❌ **CANNOT** skip validation (every block must be cryptographically checked)
- ❌ **CANNOT** estimate sync time reliably (network-dependent)

### Phase 11: Lightning Integration
- ✅ Channel funding outputs are cryptographically verified
- ✅ HTLC outputs are verified with proofs
- ✅ Watchtower can validate without UTXO DB
- ❌ **CANNOT** guarantee instant verification (proof-dependent)
- ❌ **CANNOT** verify if proofs are unavailable

### Phase 12: Mobile Profile
- ✅ Mobile nodes use same validator (same security)
- ✅ Cache eviction is safe (iOS jetsam → re-fetch → continue)
- ✅ Burst mode complies with iOS background limits
- ❌ **CANNOT** validate continuously (burst-only on mobile)
- ❌ **CANNOT** serve proofs (consume-only mode)

**Summary of Guarantees:**

✅ **CAN honestly say:** "Validated to block X with cryptographic proof"
❌ **CANNOT honestly say:** "Fully synced" (sync is continuous, never "done")
❌ **CANNOT honestly say:** "Safe" (safety requires ongoing validation)

---

## Part A: UX Truth Tables

This section defines **exactly** what the UI can show based on validator state.

### A.1: Validation State Representation

| Validator State | Honest UI Text | Dishonest UI Text (FORBIDDEN) |
|----------------|----------------|-------------------------------|
| Block X validated with proof | "Validated to block X" | "Synced" |
| Waiting for proofs Y-Z | "Awaiting proofs for blocks Y-Z" | "Syncing..." |
| Offline (no network) | "Validation paused (offline)" | "Error: sync failed" |
| Cache evicted (iOS jetsam) | "Re-fetching proofs" | "Degraded mode" |
| Burst validation sleeping | "Next validation in ~Xs" | "Idle" |
| All known blocks validated | "Validated to tip (block X)" | "Fully synced" |

**Why "Synced" is Forbidden:**

"Synced" implies:
1. All blocks are validated ✅ (can be true)
2. No more work is needed ❌ (NEVER true — new blocks arrive continuously)
3. The chain is "complete" ❌ (chains grow forever)

**Honest Alternative:**
- "Validated to block 850,000 (current tip)"
- "Background validation active"
- "Last validated 2 minutes ago"

### A.2: Proof Availability States

| Proof State | Honest UI Text | User Action |
|-------------|----------------|-------------|
| Proofs available in cache | "Proofs cached (fast validation)" | None |
| Proofs missing (re-fetch needed) | "Fetching proofs from network" | None (automatic) |
| Proofs unavailable (all peers offline) | "Waiting for peers (validation paused)" | "Try again when online" |
| Proof fetch timeout | "Network timeout (retrying)" | "Check connection" |

**Key Rule:** Never hide proof unavailability as a generic "error."

### A.3: Resource Constraint States (Mobile)

| Resource State | Honest UI Text | Explanation |
|----------------|----------------|-------------|
| Burst mode active | "Validating (burst mode, ~30s)" | iOS background limit |
| Burst mode sleeping | "Next validation in ~15 minutes" | Battery optimization |
| Cache eviction | "Memory limit reached (re-fetching)" | iOS memory pressure |
| Low battery | "Validation paused (battery < 20%)" | User-defined threshold |

**Key Rule:** Explain **why** validation is paused/limited.

### A.4: Lightning Verification States

| Lightning Operation | Honest UI Text | Forbidden UI Text |
|---------------------|----------------|-------------------|
| Channel funding verified with proof | "Channel verified ✓ (cryptographic proof)" | "Channel open" |
| Channel funding unverified (waiting proof) | "Channel opening (awaiting proof)" | "Channel confirmed" |
| HTLC verified with proof | "Payment verified ✓ (math-backed)" | "Payment complete" |
| Watchtower validation active | "Watchtower monitoring (stateless)" | "Protected" |

**Why "Channel Open" is Insufficient:**

"Channel open" could mean:
1. Funding TX broadcast ❌ (not verified)
2. Funding TX in mempool ❌ (not confirmed)
3. Funding TX in block ❌ (not validated by this node)
4. Funding TX cryptographically verified ✅ (this is what we prove)

**Honest Alternative:**
- "Channel funding verified with proof (block 850,123)"
- "Watchtower monitoring active (stateless verification)"

---

## Part B: Deployment Profile Identities

Each profile has a **public identity** that accurately represents its capabilities.

### B.1: Profile Identity Strings

| Profile | Public Name | Short Description | User-Facing Role |
|---------|-------------|-------------------|------------------|
| Full Node | "Archive Validator" | "Stores all blockchain history" | "Running a full archive node" |
| Stateless Node | "Low-Storage Validator" | "Full validation without UTXO DB" | "Running a stateless validator" |
| Mobile Node | "Phone Validator" | "Full validation on mobile device" | "Validating on your phone" |
| Lightning Node | "Payment Node" | "Lightning payments with proof verification" | "Lightning payment node" |
| Watchtower | "Guardian Node" | "Monitors Lightning channels (stateless)" | "Running a watchtower" |

**Design Rule:** Never use "light" or "SPV" — these imply reduced security.

### B.2: Profile Capability Matrix

What each profile **can honestly claim**:

| Capability | Full Node | Stateless Node | Mobile Node | Lightning Node | Watchtower |
|------------|-----------|----------------|-------------|----------------|------------|
| Cryptographic validation | ✅ | ✅ | ✅ | ✅ | ✅ |
| Zero trust assumptions | ✅ | ✅ | ✅ | ✅ | ✅ |
| Proof-based consensus | ✅ | ✅ | ✅ | ✅ | ✅ |
| Continuous validation | ✅ | ✅ | ❌ (burst) | ✅ | ✅ |
| Serve proofs to peers | ✅ | ✅ | ❌ | ✅ | ❌ |
| Lightning support | ✅ | ✅ | ✅ | ✅ | ✅ |
| Offline-friendly | ❌ | ❌ | ✅ | ❌ | ❌ |

**Key Insight:** Mobile nodes are **not** "light clients" — they are **full validators** operating in burst mode.

### B.3: Profile Comparison UX

When a user asks "What profile should I run?", the UI must **honestly** present trade-offs:

**Archive Validator (Full Node):**
- ✅ Stores full blockchain history
- ✅ Highest proof availability (helps network)
- ❌ ~500 GB storage required
- ❌ Desktop/server only

**Low-Storage Validator (Stateless Node):**
- ✅ Full validation without UTXO DB
- ✅ ~100-200 MB storage (headers only)
- ✅ Serves proofs to network
- ❌ Requires reliable network connection

**Phone Validator (Mobile Node):**
- ✅ Same security as desktop (same validator)
- ✅ Works on iOS/Android
- ✅ Burst mode (battery-friendly)
- ❌ Request-only (doesn't serve proofs)
- ❌ Periodic validation (not continuous)

**Payment Node (Lightning):**
- ✅ Instant Lightning payments
- ✅ Cryptographic channel verification
- ✅ Stateless watchtower support
- ❌ Requires always-online (for payments)

**Guardian Node (Watchtower):**
- ✅ Monitors Lightning channels
- ✅ No UTXO DB required (stateless)
- ✅ Low resource usage
- ❌ Specialized use (not general-purpose)

**Design Rule:** Never hide the trade-offs. Users deserve honesty.

---

## Part C: Failure Transparency Rules

This section defines what failures **must never** be hidden from users.

### C.1: Network Failures

| Failure Type | Honest UI Behavior | Forbidden UI Behavior |
|--------------|--------------------|-----------------------|
| All peers offline | "No peers available (validation paused)" | Silent failure |
| Proof fetch timeout | "Proof request timed out (retrying in 5s)" | "Syncing..." (hides issue) |
| Peer sent invalid proof | "Invalid proof rejected (trying different peer)" | "Sync error" (too vague) |

**Key Rule:** Explain **what** failed and **what** is being done about it.

### C.2: Resource Failures

| Failure Type | Honest UI Behavior | Forbidden UI Behavior |
|--------------|--------------------|-----------------------|
| Cache full (eviction needed) | "Cache full (evicting old proofs)" | Silent eviction |
| iOS memory pressure | "Memory pressure (clearing cache)" | Silent jetsam |
| Low battery (paused) | "Validation paused (battery < 20%)" | Silent pause |

**Key Rule:** Resource constraints are **not errors** — they are **operating conditions**.

### C.3: Validation Failures

| Failure Type | Honest UI Behavior | Forbidden UI Behavior |
|--------------|--------------------|-----------------------|
| Block fails validation | "Block X rejected (invalid proof)" | "Sync failed" |
| Accumulator mismatch | "Block X rejected (accumulator mismatch)" | "Error" |
| Proof missing for block | "Block X unverifiable (proof unavailable)" | "Sync stalled" |

**Key Rule:** Distinguish between:
- **Validation failure** (block is provably invalid) ← Security feature
- **Verification unavailable** (waiting for proof) ← Temporary state

### C.4: Lightning Failures

| Failure Type | Honest UI Behavior | Forbidden UI Behavior |
|--------------|--------------------|-----------------------|
| Channel funding unverified | "Channel pending (awaiting proof)" | "Channel opening..." |
| HTLC unverifiable | "Payment pending (awaiting proof)" | "Payment processing..." |
| Watchtower offline | "Watchtower offline (manual monitoring recommended)" | Silent failure |

**Key Rule:** Never show "verified" until cryptographic proof is available.

---

## Part D: Mobile UX Contracts

Mobile nodes make **explicit contracts** with users about time, battery, and resources.

### D.1: Battery Consumption Contract

**Contract Statement:**
> "Background validation will consume approximately **1-3% battery per day**."

**What This Guarantees:**
- ✅ Burst validation limited to 30 seconds
- ✅ Sleep between bursts (~15 minutes)
- ✅ No continuous background network activity
- ✅ No unsolicited proof serving

**How to Measure:**
- iOS: Energy Impact API
- Android: Battery Stats API

**UI Display:**
```
Battery Impact: Low (~2% daily)
Last validation: 5 minutes ago
Next validation: ~10 minutes
```

### D.2: Sync Time Contract

**Contract Statement:**
> "Estimated time to validate 10,000 blocks: **~30-60 minutes** (background mode)."

**What This Guarantees:**
- ✅ Honest estimate based on burst mode (30s validation, 15min sleep)
- ✅ Accounts for network latency
- ✅ Accounts for proof availability

**What This Does NOT Guarantee:**
- ❌ **NOT** a promise (network-dependent)
- ❌ **NOT** a deadline (can take longer)
- ❌ **NOT** a requirement (users can pause)

**UI Display:**
```
Progress: 8,500 / 10,000 blocks validated
Estimated time remaining: ~20 minutes
(network-dependent, may vary)
```

### D.3: Data Usage Contract

**Contract Statement:**
> "Validating 1,000 blocks will consume approximately **1-2 MB of data**."

**What This Guarantees:**
- ✅ Proofs are ~1-2 KB each
- ✅ Headers are ~80 bytes each
- ✅ No unsolicited gossip (request-only)

**UI Display:**
```
Data usage (estimated): ~1.5 MB / 1,000 blocks
Total data used: 15 MB
```

### D.4: Background Execution Contract (iOS)

**Contract Statement:**
> "Background validation runs in **30-second bursts** to comply with iOS limits."

**What This Guarantees:**
- ✅ Each burst completes within 30 seconds
- ✅ Sleep between bursts (iOS schedules next wake)
- ✅ Validation resumes automatically

**UI Display:**
```
Background Mode: Active
Burst duration: 30 seconds max
Next burst: ~12 minutes
```

---

## Part E: Lightning UX Contracts

Lightning operations make **specific claims** about what "verified" means.

### E.1: Channel Funding Verification

**Contract Statement:**
> "Channel funding is **cryptographically verified with a Utreexo proof**."

**What This Proves:**
- ✅ Funding TX is in a validated block
- ✅ Funding output exists in the accumulator
- ✅ Funding amount matches channel capacity
- ✅ No trust in block explorers or third parties

**What This Does NOT Prove:**
- ❌ **NOT** that the channel will remain open
- ❌ **NOT** that the peer is honest
- ❌ **NOT** that payments will succeed

**UI Display:**
```
Channel Status: Verified ✓
Funding TX: abc123...
Block height: 850,123
Proof verified: Yes (cryptographic)
Capacity: 1,000,000 sats
```

### E.2: HTLC Settlement Verification

**Contract Statement:**
> "HTLC settlement is **verified with math-backed proofs**."

**What This Proves:**
- ✅ HTLC output exists in accumulator
- ✅ Preimage matches hash (on reveal)
- ✅ Timeout is valid (on timeout)

**UI Display:**
```
Payment Status: Verified ✓
HTLC TX: def456...
Settlement type: Preimage reveal
Proof verified: Yes
Amount: 50,000 sats
```

### E.3: Watchtower Monitoring

**Contract Statement:**
> "Watchtower monitoring is **stateless** (no UTXO database required)."

**What This Proves:**
- ✅ Watchtower validates breach TXs with proofs
- ✅ No trust in watchtower operator
- ✅ Offline-friendly (watchtower fetches proofs)

**UI Display:**
```
Watchtower Status: Monitoring ✓
Mode: Stateless (proof-based)
Channels monitored: 3
Last check: 2 minutes ago
```

### E.4: Lightning "Verified" State Machine

| State | UI Text | Meaning |
|-------|---------|---------|
| Channel opening | "Awaiting funding TX proof" | Funding TX broadcast, waiting for proof |
| Channel verified | "Channel verified ✓ (proof confirmed)" | Funding TX cryptographically verified |
| Channel active | "Channel active (verified)" | Channel verified and ready for payments |
| Channel closing | "Closing (awaiting settlement proof)" | Closing TX broadcast, waiting for proof |
| Channel closed | "Channel closed ✓ (settlement verified)" | Closing TX cryptographically verified |

**Key Rule:** "Verified" requires **cryptographic proof**, not just "seen on network."

---

## Part F: Operator Documentation

This section defines what **node operators** see (not end users).

### F.1: Deployment Checklist

**Full Node (Archive Validator):**
```
[ ] 500+ GB storage available
[ ] Reliable network connection
[ ] Port 8333 open (P2P)
[ ] systemd service configured
[ ] Automatic updates enabled
[ ] Monitoring dashboard configured
```

**Stateless Node (Low-Storage Validator):**
```
[ ] 200 MB storage available
[ ] Reliable network connection
[ ] Bridge node peers configured
[ ] Proof cache tuned (100 MB default)
[ ] Proof gossip enabled
[ ] Monitoring active
```

**Mobile Node (Phone Validator):**
```
[ ] iOS 15+ or Android 10+
[ ] Background app refresh enabled
[ ] Low Power Mode disabled (for sync)
[ ] Wi-Fi preferred (for initial sync)
[ ] Notifications enabled (for errors)
[ ] Battery optimization disabled (for app)
```

**Lightning Node (Payment Node):**
```
[ ] Always-online server
[ ] Tor configured (optional)
[ ] Watchtower configured
[ ] Channel backups automated
[ ] Proof cache tuned for Lightning (8 MB default)
[ ] Monitoring active
```

### F.2: Health Check Dashboard

What operators should see:

```
=== Validator Health ===
Status: Validated to block 850,123 (tip)
Last validation: 30 seconds ago
Proof cache: 45 MB / 100 MB (45% full)
Peers: 8 connected (4 bridge nodes)
Network: 24 KB/s down, 12 KB/s up

=== Lightning Health ===
Channels: 5 active (all verified ✓)
Watchtower: Monitoring (stateless)
Pending HTLCs: 0
Last channel update: 5 minutes ago

=== Resource Usage ===
Memory: 120 MB (proof cache: 45 MB)
CPU: 2% average
Disk: 150 MB (headers only)
Battery: Not applicable (desktop)
```

### F.3: Error Log Format

All errors must be **actionable**:

**Good Error Log:**
```
[ERROR] Block 850,124 validation failed
  Reason: Accumulator root mismatch
  Expected: abc123...
  Received: def456...
  Action: Verify proof from different peer
```

**Bad Error Log (FORBIDDEN):**
```
[ERROR] Sync failed
```

**Key Rule:** Every error must explain:
1. **What** happened
2. **Why** it happened
3. **What** will be done about it

---

## Part G: App Store Compliance Mapping

This section maps Phase 13 contracts to App Store approval language.

### G.1: iOS App Store Submission

**App Description (Honest):**
```
DineroCoin Phone Validator

Full blockchain validation on your iPhone.

• Same security as desktop nodes (cryptographic proofs)
• No UTXO database (headers only, ~150 MB)
• Background sync in 30-second bursts (battery-friendly)
• Lightning channel verification (math-backed)
• Stateless watchtower support

Background Usage:
This app validates blockchain headers in the background
to keep your wallet synced. Background execution is limited
to 30-second bursts with ~15 minutes of sleep between bursts.

Estimated battery impact: 1-3% per day.

This is NOT a light client — this is a full validator
running in burst mode.
```

**Background Modes Justification:**
```
Background Fetch: Used to validate blockchain headers
in compliance with iOS 30-second background execution limits.

Each background session:
- Validates up to 50 block headers
- Fetches required Utreexo proofs
- Updates validation state
- Completes within 30 seconds

This enables the app to maintain blockchain sync without
requiring the app to be open.
```

### G.2: Android Play Store Submission

**App Description (Honest):**
```
DineroCoin Phone Validator

Full blockchain validation on Android.

• Same security as desktop (cryptographic proofs)
• No UTXO database (headers only, ~150 MB)
• Doze-friendly background sync
• Lightning channel verification
• Battery-optimized burst mode

Battery Impact: Low (~2% daily)

Permissions:
• INTERNET: Fetch blockchain headers and proofs
• FOREGROUND_SERVICE: Background validation
• WAKE_LOCK: Complete validation bursts
```

**Background Service Justification:**
```
This app runs a foreground service to validate blockchain
headers while syncing. The service:

- Operates in 30-second bursts
- Sleeps between bursts (Doze-friendly)
- Shows persistent notification during sync
- Stops automatically when synced

Battery optimization: Enabled (respects Doze mode)
```

### G.3: Compliance Checklist

**iOS Compliance:**
```
[ ] Background execution < 30 seconds per burst
[ ] Memory usage < 50 MB (Phase 12: 26 MB total)
[ ] No continuous background network activity
[ ] Background mode justification provided
[ ] Battery impact disclosed (1-3% daily)
[ ] Data usage disclosed (~1-2 MB per 1k blocks)
[ ] No misleading claims ("full validator" not "light client")
```

**Android Compliance:**
```
[ ] Doze mode compatible (burst mode works)
[ ] Battery optimization enabled (not requesting exemption)
[ ] Foreground service notification shown
[ ] Permissions justified in description
[ ] Battery impact disclosed
[ ] No misleading "always synced" claims
```

---

## Part H: UX State Machine

This section defines the **complete state machine** for user-facing validation state.

### H.1: State Definitions

```
State: UNINITIALIZED
- Meaning: App first launch, no headers yet
- UI: "Setting up validator..."
- Transitions to: SYNCING_HEADERS

State: SYNCING_HEADERS
- Meaning: Downloading block headers
- UI: "Syncing headers (X / Y blocks)"
- Transitions to: AWAITING_PROOFS

State: AWAITING_PROOFS
- Meaning: Headers synced, fetching proofs
- UI: "Fetching proofs for blocks X-Y"
- Transitions to: VALIDATING

State: VALIDATING
- Meaning: Running cryptographic validation
- UI: "Validating blocks (X / Y complete)"
- Transitions to: SYNCED_TO_TIP

State: SYNCED_TO_TIP
- Meaning: All known blocks validated
- UI: "Validated to tip (block X)"
- Transitions to: AWAITING_NEW_BLOCKS

State: AWAITING_NEW_BLOCKS
- Meaning: Waiting for new blocks to arrive
- UI: "Validated to tip (last update: 5 min ago)"
- Transitions to: SYNCING_HEADERS (when new block arrives)

State: PAUSED_OFFLINE
- Meaning: No network connection
- UI: "Validation paused (offline)"
- Transitions to: SYNCING_HEADERS (when online)

State: PAUSED_BATTERY
- Meaning: Low battery (user threshold)
- UI: "Validation paused (battery < 20%)"
- Transitions to: SYNCING_HEADERS (when charged)

State: PAUSED_BURST_SLEEP
- Meaning: Mobile burst mode sleeping
- UI: "Next validation in ~10 minutes"
- Transitions to: VALIDATING (on wake)

State: ERROR_VALIDATION_FAILED
- Meaning: Block failed validation
- UI: "Block X rejected (invalid proof)"
- Action: "Try different peers"
- Transitions to: SYNCING_HEADERS (on retry)

State: ERROR_PROOF_UNAVAILABLE
- Meaning: No peers have proof
- UI: "Proof unavailable for block X"
- Action: "Waiting for peers..."
- Transitions to: AWAITING_PROOFS (on peer availability)
```

### H.2: State Transition Rules

**Rule 1: Never skip states**
- ❌ UNINITIALIZED → SYNCED_TO_TIP (impossible, dishonest)
- ✅ UNINITIALIZED → SYNCING_HEADERS → AWAITING_PROOFS → VALIDATING → SYNCED_TO_TIP

**Rule 2: Never hide intermediate states**
- ❌ Show "Syncing..." for both SYNCING_HEADERS and AWAITING_PROOFS (ambiguous)
- ✅ Show exact state: "Syncing headers" vs "Fetching proofs"

**Rule 3: Always explain pauses**
- ❌ PAUSED (no explanation)
- ✅ PAUSED_OFFLINE, PAUSED_BATTERY, PAUSED_BURST_SLEEP (specific reason)

### H.3: Progress Indicators

**Header Sync Progress:**
```
Syncing headers: 840,000 / 850,000 (98%)
Estimated time: ~5 minutes
```

**Proof Fetch Progress:**
```
Fetching proofs: 840,000 / 850,000 (98%)
Network speed: 24 KB/s
Estimated time: ~10 minutes
```

**Validation Progress:**
```
Validating blocks: 840,000 / 850,000 (98%)
Proofs verified: 100%
Estimated time: ~3 minutes
```

**Key Rule:** "Estimated time" is always labeled as an estimate, never a guarantee.

---

## Part I: Implementation Checklist

Phase 13 is **not just documentation** — it requires code changes.

### I.1: Required Components

**Component 1: Validation State Manager**
```cpp
class ValidationStateManager {
public:
    enum class State {
        UNINITIALIZED,
        SYNCING_HEADERS,
        AWAITING_PROOFS,
        VALIDATING,
        SYNCED_TO_TIP,
        AWAITING_NEW_BLOCKS,
        PAUSED_OFFLINE,
        PAUSED_BATTERY,
        PAUSED_BURST_SLEEP,
        ERROR_VALIDATION_FAILED,
        ERROR_PROOF_UNAVAILABLE
    };

    State GetCurrentState() const;
    std::string GetUserFacingText() const;
    std::optional<std::string> GetActionableMessage() const;
};
```

**Component 2: Progress Tracker**
```cpp
class ProgressTracker {
public:
    struct Progress {
        uint32_t current_block;
        uint32_t target_block;
        double percentage;
        std::optional<uint64_t> estimated_time_ms;  // Optional, may be unknown
    };

    Progress GetHeaderSyncProgress() const;
    Progress GetProofFetchProgress() const;
    Progress GetValidationProgress() const;
};
```

**Component 3: Resource Monitor**
```cpp
class ResourceMonitor {
public:
    struct ResourceUsage {
        uint64_t memory_bytes;
        uint64_t cache_bytes;
        double battery_percent;
        uint64_t data_used_bytes;
    };

    ResourceUsage GetCurrentUsage() const;
    bool ShouldPauseForBattery() const;
    bool ShouldPauseForMemory() const;
};
```

**Component 4: Honest UI Bridge**
```cpp
class HonestUIBridge {
public:
    // Get current state as user-facing text
    std::string GetStatusText() const;

    // Get actionable message (if any)
    std::optional<std::string> GetActionMessage() const;

    // Get progress (if applicable)
    std::optional<ProgressTracker::Progress> GetProgress() const;

    // Get resource usage
    ResourceMonitor::ResourceUsage GetResourceUsage() const;

    // Verify state is honest (debug/testing)
    bool VerifyStateHonesty() const;
};
```

### I.2: Testing Requirements

**Test 1: State Transition Honesty**
```cpp
void test_state_transitions_are_honest() {
    // Verify states follow rules (never skip)
    // Verify text matches state (never lies)
    // Verify progress is monotonic (never regresses)
}
```

**Test 2: Failure Transparency**
```cpp
void test_failures_are_transparent() {
    // Verify offline state is shown
    // Verify proof unavailability is shown
    // Verify validation failures are shown (not hidden)
}
```

**Test 3: Resource Contract Compliance**
```cpp
void test_resource_contracts() {
    // Verify burst mode < 30 seconds
    // Verify battery impact < 3% daily
    // Verify data usage matches estimate
}
```

---

## Part J: What This Enables

### J.1: Unique Claims DineroCoin Can Make

**Claim 1: "Full validation on your phone"**
- ✅ Provably true (same validator as desktop)
- ✅ No asterisks (not SPV, not light client)
- ✅ No trust assumptions (cryptographic proofs)

**Claim 2: "Math-backed Lightning verification"**
- ✅ Provably true (Utreexo proofs)
- ✅ No block explorers needed (self-validated)
- ✅ No UTXO database needed (stateless)

**Claim 3: "iOS-compliant burst validation"**
- ✅ Provably true (compile-time enforced < 30s)
- ✅ No jailbreak needed (works within iOS limits)
- ✅ No background mode hacks (legitimate use)

**Claim 4: "Honest failure reporting"**
- ✅ Provably true (state machine enforces honesty)
- ✅ No hidden errors (all failures explained)
- ✅ No fake sync status (never lies about validation)

### J.2: What Other Projects Cannot Honestly Claim

**Most "Mobile Wallets":**
- ❌ "Synced" (SPV nodes are never fully synced)
- ❌ "Verified" (trust block explorers, not proofs)
- ❌ "Full validation" (bloom filters ≠ validation)

**Most Lightning Wallets:**
- ❌ "Channel verified" (trust block explorers)
- ❌ "No UTXO DB needed" (watchtowers need full UTXO set)
- ❌ "Stateless watchtower" (not actually stateless)

**Most Blockchain Apps:**
- ❌ "Background sync" (iOS usually kills these)
- ❌ "Low battery impact" (continuous sync drains battery)
- ❌ "Honest about failures" (errors are hidden/vague)

---

## Conclusion

**What Phase 13 Achieves:**

1. **Architectural Honesty:** UI cannot lie about validator state
2. **Deployment Clarity:** Each profile has accurate public identity
3. **Failure Transparency:** All errors are explained, never hidden
4. **Resource Contracts:** Explicit guarantees about battery/time/data
5. **Lightning Truthfulness:** "Verified" means cryptographic proof, not trust

**What Phase 13 Does NOT Change:**

- ❌ Consensus logic (still Phase 8)
- ❌ Proof cryptography (still Phase 9)
- ❌ Sync algorithm (still Phase 10)
- ❌ Lightning integration (still Phase 11)
- ❌ Mobile profile (still Phase 12)

**Core Theorem:**

> "A validator that cannot prove correctness must not claim correctness."

This is the architectural discipline that prevents DineroCoin from becoming dishonest.

---

**Next Steps:**

1. Implement `ValidationStateManager` (honest state tracking)
2. Implement `ProgressTracker` (honest progress reporting)
3. Implement `ResourceMonitor` (honest resource tracking)
4. Implement `HonestUIBridge` (translation layer)
5. Write Phase 13 tests (verify honesty guarantees)
6. Map to platform-specific UI (iOS, Android, desktop)
7. Submit to App Store with honest descriptions

**Phase 13 Status:** Design Complete → Implementation Ready

