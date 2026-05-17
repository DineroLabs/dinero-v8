# Phase O — Network Invariants (LOCKED)

**Status:** ✅ COMPLETE
**Commit:** 583eb2ec
**Date:** 2025-12-21
**Classification:** Sensors, NOT Actuators

---

## What Was Done

### 1. Detection System (Observation Only)
- ✅ Implemented 9 network invariant checks
- ✅ Periodic checking every 60 seconds in maintenance loop
- ✅ RPC diagnostic command `checknetworkinvariants`
- ✅ Logging of violations with severity levels

### 2. Invariants Checked
1. **Connection count consistency** - NetworkManager vs ConnectionManager counts match
2. **Connection limits** - Total connections within MAX_TOTAL bounds
3. **Eviction protection** - Minimum protected outbound peers maintained
4. **Subnet diversity** - Eclipse attack detection (max 32 peers per /16 subnet)
5. **Duplicate peer detection** - Each peer registered exactly once
6. **Socket state consistency** - No orphaned sockets or ghost peers
7. **Inbound/outbound balance** - Proper categorization of connections
8. **Protected peer verification** - Protected peers actually exist
9. **Connection slot integrity** - Slot accounting matches reality

### 3. What This System Does
- **Detects** violations and logs them
- **Reports** violations via RPC for diagnostics
- **Monitors** network health passively
- **Alerts** operators to potential attacks

---

## What Was Intentionally NOT Done

### ❌ NO Enforcement Actions
- **NO** auto-disconnect logic
- **NO** peer scoring penalties
- **NO** peer banning
- **NO** connection slot rebalancing
- **NO** automatic remediation of any kind

### ❌ NO Policy Decisions
- Does NOT decide what to do when violations occur
- Does NOT modify network state
- Does NOT kill connections
- Does NOT adjust peer scoring

### ❌ NO Side Effects
- Pure observation system
- Read-only access to network state
- Logging only, no state mutations
- Thread-safe read operations only

---

## Why Invariants Are Sensors, Not Actuators

### Separation of Concerns
- **Detection** (Phase O) ≠ **Response** (Future Phase)
- Allows policy changes without touching detection logic
- Easier to test, audit, and reason about
- Prevents unintended consequences

### Engineering Discipline
- Mixing detection + enforcement = complex, brittle systems
- Hard to debug when detection triggers wrong action
- Policy evolution requires touching core detection code
- Testing becomes exponentially harder

### Future Flexibility
- Response logic can evolve independently
- Different deployments can have different policies
- A/B testing of remediation strategies possible
- Gradual rollout of enforcement actions

---

## Enforcement Belongs in Future Phases

When (and if) enforcement is needed, create a NEW phase:

### Phase P (Example) — Network Invariant Enforcement
- Consume violation events from Phase O
- Implement graduated response system
- Add peer scoring integration
- Implement connection eviction policies
- Add manual override capabilities
- Comprehensive logging of enforcement actions

### Design Principles for Future Enforcement
1. **Graduated Response**
   - First violation: Log only
   - Repeated violations: Increase peer misbehavior score
   - Persistent violations: Temporary cooldown
   - Severe violations: Disconnect + ban

2. **Manual Override**
   - Operator can disable auto-enforcement
   - Whitelist trusted peers
   - Override specific invariant checks

3. **Audit Trail**
   - Log every enforcement action
   - Include full context (which invariant, severity, peer info)
   - Enable post-mortem analysis

4. **Metrics & Monitoring**
   - Count violations by type
   - Track false positive rates
   - Measure enforcement effectiveness

---

## Files Modified (Phase O)

```
include/daemon/network_manager.h           (getConnectionManager() added)
include/dinero/network/network_invariants.h (namespace fixed)
src/daemon/network_manager.cpp             (periodic checking added)
src/rpc/methods_network_context.cpp        (RPC command added)
```

---

## Do NOT Modify This Phase For

- Adding auto-disconnect
- Adding peer scoring
- Adding enforcement actions
- Adding remediation logic
- Adding state mutations

## Safe Modifications

- Bug fixes in detection logic
- Additional invariant checks (detection only)
- Performance optimizations (read-only)
- Additional logging/metrics
- RPC response format improvements

---

## Testing Checklist (Future)

When writing tests for enforcement (Phase P):
- [ ] Test detection in isolation first
- [ ] Mock violation events, don't rely on Phase O internals
- [ ] Test each enforcement action independently
- [ ] Test graduated response escalation
- [ ] Test manual override capabilities
- [ ] Test false positive handling
- [ ] Load test enforcement under high violation rates

---

## Key Invariants of This Phase (Meta)

1. **No state mutations** - Invariant checks are pure functions of network state
2. **No side effects** - Beyond logging, system is read-only
3. **No policy** - Detection does not imply action
4. **Thread-safe** - Read-only operations are inherently safe
5. **Observable** - All violations are logged and queryable

---

## Commit History

| Commit | Description |
|--------|-------------|
| 583eb2ec | Phase O: Network invariants integration - RPC exposure and periodic checking |
| [previous] | Phase O: Complete network invariants implementation (9 checks) |
| [previous] | Phase O: Add RPC metadata for blockchain methods |

---

## Lock Date: 2025-12-21

**This phase is now LOCKED. Do not extend with enforcement logic.**

If you need enforcement, create Phase P (or similar) as a separate, well-scoped phase.

---

## Usage

### Manual RPC Check
```bash
dinero-cli checknetworkinvariants
```

### Expected Output (All Good)
```json
[
  {
    "status": "ok",
    "message": "All network invariants passed"
  }
]
```

### Expected Output (Violations Detected)
```json
[
  {
    "invariant": "subnet_diversity_violation",
    "description": "Subnet 192.168.1.0/16 has 45 peers (max: 32). Peers: [...]. Possible eclipse attack!",
    "severity": "WARNING"
  },
  {
    "invariant": "duplicate_peer_registration",
    "description": "Peer 192.168.1.100:20999 is registered 2 times. Expected exactly 1 registration per peer.",
    "severity": "CRITICAL"
  }
]
```

### Automatic Checking
- Runs every 60 seconds in NetworkManager::peerMaintenanceThread()
- Logs to dinero.log with [NETWORK_INVARIANT] prefix
- No action taken, only observation and logging

---

**🔒 PHASE O IS LOCKED. SENSORS, NOT ACTUATORS.**
