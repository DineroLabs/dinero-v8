# DPI Payment State Machine

## Version 0.1

---

## 1. States

```
┌─────────────────────────────────────────────────────────────────┐
│                        DPI Payment Lifecycle                     │
└─────────────────────────────────────────────────────────────────┘

                         ┌──────────┐
                         │   INIT   │
                         └────┬─────┘
                              │ scan QR / enter request
                              ▼
                         ┌──────────┐
                         │ REQUESTED│◄─────────────────┐
                         └────┬─────┘                  │
                              │ build tx + proof       │ retry
                              ▼                        │ (new intent)
                         ┌──────────┐                  │
                         │  BUILT   │──────────────────┘
                         └────┬─────┘        ▲
                              │ send         │ rebuild
                              ▼              │
┌───────────────────────────────────────────────────────────────┐
│ SUBMITTED                                                      │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │                      VERIFYING                           │   │
│ │  ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐      │   │
│ │  │ V_REQ  │──▶│ V_INT  │──▶│ V_UTX  │──▶│ V_CT   │──┐   │   │
│ │  └────────┘   └────────┘   └────────┘   └────────┘  │   │   │
│ │       │            │            │            │      │   │   │
│ │       ▼            ▼            ▼            ▼      │   │   │
│ │    [fail]       [fail]       [fail]       [fail]    │   │   │
│ │       │            │            │            │      │   │   │
│ │       └────────────┴────────────┴────────────┘      │   │   │
│ │                         │                           │   │   │
│ │                         ▼                           ▼   │   │
│ │                   ┌──────────┐              ┌─────────┐ │   │
│ │                   │ REJECTED │              │ACCEPTED │ │   │
│ │                   │(terminal)│              │ TIER_1  │ │   │
│ │                   └──────────┘              └────┬────┘ │   │
│ └─────────────────────────────────────────────────│──────┘   │
└───────────────────────────────────────────────────│──────────┘
                                                    │
                              ┌─────────────────────┘
                              │ broadcast + observe
                              ▼
                    ┌───────────────────┐
                    │  ACCEPTED_TIER_2  │◄──────┐
                    └─────────┬─────────┘       │
                              │                 │ conflict detected
                              │                 │ late (rare)
                              ▼                 │
                    ┌───────────────────┐       │
                    │     CONFIRMING    │───────┘
                    └─────────┬─────────┘
                              │ depth >= required
                              ▼
                    ┌───────────────────┐
                    │  SETTLED (final)  │
                    └───────────────────┘
```

---

## 2. State Definitions

| State | Owner | Description |
|-------|-------|-------------|
| `INIT` | Sender | No payment in progress |
| `REQUESTED` | Sender | PaymentRequest received, awaiting user review |
| `BUILT` | Sender | PaymentIntent constructed, not yet sent |
| `SUBMITTED` | Both | Intent sent, receiver processing |
| `VERIFYING` | Receiver | Deterministic verification pipeline running |
| `ACCEPTED_TIER_1` | Receiver | Crypto verification passed, pre-network |
| `ACCEPTED_TIER_2` | Receiver | Network observation passed |
| `CONFIRMING` | Receiver | Awaiting block depth |
| `SETTLED` | Both | **Terminal:** Payment finalized |
| `REJECTED` | Both | **Terminal:** Payment failed |
| `EXPIRED` | Both | **Terminal:** Request timed out |

---

## 3. Verification Sub-Pipeline (V_*)

VERIFYING is not atomic — it's a deterministic sequence:

```
V_REQ ──▶ V_INT ──▶ V_UTX ──▶ V_CT ──▶ V_POL
  │         │         │         │         │
  ▼         ▼         ▼         ▼         ▼
ERR_REQ_* ERR_INT_* ERR_UTX_* ERR_CT_* ERR_POL_*
```

**Rules:**
- Execute in order, left to right
- Stop on first failure
- Return single error code
- No partial acceptance

```
function verify(request, intent) -> Result<(), Error>:
    validate_request(request)?      // ERR_REQ_*
    validate_intent(intent)?        // ERR_INT_*
    verify_utreexo(intent.proof)?   // ERR_UTX_*
    verify_ct(intent, request)?     // ERR_CT_*
    check_policy(intent, request)?  // ERR_POL_*
    return Ok(())
```

---

## 4. Transition Table

### 4.1 Happy Path

| From | To | Trigger | Errors | Notes |
|------|----|---------|--------|-------|
| `INIT` | `REQUESTED` | Scan QR / receive request | — | Sender-local |
| `REQUESTED` | `BUILT` | User confirms, wallet builds intent | `ERR_REQ_*`, `ERR_SYS_003`, `ERR_SYS_004` | Sender-local |
| `BUILT` | `SUBMITTED` | Send intent to receiver | `ERR_SYS_005` | Network required |
| `SUBMITTED` | `VERIFYING` | Receiver receives intent | — | Immediate |
| `VERIFYING` | `ACCEPTED_TIER_1` | All V_* checks pass | — | Deterministic |
| `ACCEPTED_TIER_1` | `ACCEPTED_TIER_2` | Broadcast + no conflict + propagation | `ERR_NET_*` | Time-bounded |
| `ACCEPTED_TIER_2` | `CONFIRMING` | Awaiting confirmations | — | Policy-driven |
| `CONFIRMING` | `SETTLED` | `depth >= required_confirmations` | — | Monotonic |

### 4.2 Failure Paths

| From | To | Trigger | Error Code |
|------|----|---------|------------|
| `REQUESTED` | `EXPIRED` | `now > request.expires_at` | `ERR_REQ_001_EXPIRED` |
| `BUILT` | `EXPIRED` | `now > request.expires_at` | `ERR_REQ_001_EXPIRED` |
| `VERIFYING` | `REJECTED` | Any V_* fails | `ERR_*` (specific) |
| `ACCEPTED_TIER_1` | `REJECTED` | Broadcast fails | `ERR_NET_001_BROADCAST_FAILED` |
| `ACCEPTED_TIER_1` | `REJECTED` | Conflict detected | `ERR_NET_002_CONFLICT_DETECTED` |
| `ACCEPTED_TIER_1` | `REJECTED` | Insufficient propagation | `ERR_NET_003_INSUFFICIENT_PROPAGATION` |
| `ACCEPTED_TIER_2` | `REJECTED` | Late conflict (rare) | `ERR_NET_002_CONFLICT_DETECTED` |
| `CONFIRMING` | `REJECTED` | Reorg eliminates tx | `ERR_NET_006_REORG_DETECTED` |

### 4.3 Retry Paths

| From | To | Trigger | Notes |
|------|----|---------|-------|
| `REJECTED` | `REQUESTED` | User initiates new payment | New `request_id` required |
| `BUILT` | `BUILT` | Rebuild with fresh root | Same `request_id` allowed |

---

## 5. Dual Perspective View

Sender and receiver see different states:

```
Timeline ──────────────────────────────────────────────────────▶

SENDER VIEW:
  INIT ──▶ REQUESTED ──▶ BUILT ──▶ SUBMITTED ──▶ AWAITING ──▶ DONE
                                        │
                                        │ (sender waits)
                                        ▼
RECEIVER VIEW:                    VERIFYING
                                      │
                        ┌─────────────┼─────────────┐
                        ▼             ▼             ▼
                    REJECTED    ACCEPTED_T1    ACCEPTED_T1
                                      │             │
                                      ▼             ▼
                                ACCEPTED_T2    REJECTED
                                      │
                                      ▼
                                 CONFIRMING
                                      │
                                      ▼
                                   SETTLED
```

**Synchronization points:**
- `SUBMITTED`: Sender sends, receiver receives
- `REJECTED`: Receiver sends reject, sender receives
- `ACCEPTED_*`: Receiver sends ack, sender receives
- `SETTLED`: Both agree (block confirmation is public)

---

## 6. Timeout Behavior

| State | Timeout | Action |
|-------|---------|--------|
| `REQUESTED` | `request.expires_at` | → `EXPIRED` |
| `BUILT` | `request.expires_at` | → `EXPIRED` |
| `SUBMITTED` | Receiver response timeout (e.g., 30s) | Sender: retry or abort |
| `ACCEPTED_TIER_1` | Tier 2 observation window (e.g., 10s) | → `ACCEPTED_TIER_2` or `REJECTED` |
| `CONFIRMING` | None (blocks are async) | Wait indefinitely or policy timeout |

---

## 7. State Invariants

### 7.1 Global Invariants

| ID | Invariant | Violation = Bug |
|----|-----------|-----------------|
| I1 | Payment in exactly one state at any time | State corruption |
| I2 | Terminal states have no outbound transitions | State machine error |
| I3 | `request_id` unique per payment attempt | Replay vulnerability |
| I4 | State transitions are append-only (logged) | Audit failure |
| I5 | Rejection includes exactly one `ERR_*` | Debugging impossible |

### 7.2 Per-State Invariants

| State | Invariant |
|-------|-----------|
| `BUILT` | `intent.request_id == request.request_id` |
| `SUBMITTED` | Intent immutable (hash locked) |
| `ACCEPTED_TIER_1` | All V_* passed |
| `ACCEPTED_TIER_2` | No conflict observed in window |
| `SETTLED` | `confirmations >= policy.required_depth` |

### 7.3 Monotonicity Rules

```
TIER_1 < TIER_2 < CONFIRMING < SETTLED

Once ACCEPTED_TIER_2 reached:
  - Cannot return to TIER_1
  - Can only advance or REJECT (conflict/reorg)

Once SETTLED:
  - Permanent
  - Reorgs deeper than settlement depth are outside DPI scope
```

---

## 8. Role Responsibilities

### Sender MUST:

| State | Responsibility |
|-------|----------------|
| `REQUESTED` | Display request details, get user confirmation |
| `BUILT` | Ensure intent matches request exactly |
| `SUBMITTED` | Await response, handle timeout |
| `AWAITING` | Persist state for recovery |
| `SETTLED` | Mark complete in local records |

### Sender MUST NOT:

- Reuse `request_id` after `REJECTED`
- Modify intent after `SUBMITTED`
- Assume `ACCEPTED_TIER_1` means goods can be received (merchant decides)

### Receiver MUST:

| State | Responsibility |
|-------|----------------|
| `VERIFYING` | Execute V_* pipeline deterministically |
| `ACCEPTED_TIER_1` | Broadcast transaction |
| `ACCEPTED_TIER_2` | Send `payment_ack` to sender |
| `CONFIRMING` | Monitor for reorgs |
| `SETTLED` | Persist finality |
| `REJECTED` | Send `payment_reject` with `ERR_*` |

### Receiver MUST NOT:

- Accept same `request_id` twice
- Skip any V_* step
- Send `payment_ack` before `ACCEPTED_TIER_2`
- Deliver goods before policy-appropriate tier

---

## 9. Test Scenarios

| ID | Scenario | Path | Exercises |
|----|----------|------|-----------|
| T01 | Happy path, instant | INIT→...→SETTLED | All happy transitions |
| T02 | Expired request | REQUESTED→EXPIRED | Timeout handling |
| T03 | Invalid signature | VERIFYING→REJECTED | V_INT, `ERR_INT_005` |
| T04 | Stale Utreexo root | VERIFYING→REJECTED | V_UTX, `ERR_UTX_003` |
| T05 | CT amount mismatch | VERIFYING→REJECTED | V_CT, `ERR_CT_001` |
| T06 | RBF transaction | VERIFYING→REJECTED | V_INT, `ERR_INT_008` |
| T07 | Broadcast failure | TIER_1→REJECTED | `ERR_NET_001` |
| T08 | Double-spend detected | TIER_1→REJECTED | `ERR_NET_002` |
| T09 | Low propagation | TIER_1→REJECTED | `ERR_NET_003` |
| T10 | Late conflict | TIER_2→REJECTED | `ERR_NET_002` |
| T11 | Reorg during confirm | CONFIRMING→REJECTED | `ERR_NET_006` |
| T12 | Duplicate request_id | VERIFYING→REJECTED | `ERR_REQ_006` |
| T13 | Amount exceeds tier | VERIFYING→REJECTED | `ERR_POL_001` |
| T14 | Retry after reject | REJECTED→REQUESTED | Retry with new request |

---

## 10. Message Flow

```
    SENDER                                      RECEIVER
      │                                            │
      │◀─────────── PaymentRequest ───────────────│
      │         (QR scan / out-of-band)            │
      │                                            │
      │  [REQUESTED]                               │
      │  [user confirms]                           │
      │  [BUILT]                                   │
      │                                            │
      │─────────── PaymentIntent ─────────────────▶│
      │                                            │  [VERIFYING]
      │                                            │  [V_REQ → V_INT → V_UTX → V_CT → V_POL]
      │                                            │
      │                                 ┌──────────┴──────────┐
      │                                 ▼                     ▼
      │◀─────────── PaymentReject ─────│         [ACCEPTED_TIER_1]
      │         (ERR_* code)           │                     │
      │  [REJECTED]                    │          [broadcast tx]
      │                                │          [observe mempool]
      │                                │                     │
      │                                │         [ACCEPTED_TIER_2]
      │◀────────────────────────────────────── PaymentAck ───│
      │                                            │
      │  [AWAITING]                     [CONFIRMING]
      │                                            │
      │            ◀── block confirms ──▶          │
      │                                            │
      │  [SETTLED]                      [SETTLED]  │
      │                                            │
```

---

## 11. Persistence Requirements

| State | Sender Persists | Receiver Persists |
|-------|-----------------|-------------------|
| `REQUESTED` | request | — |
| `BUILT` | request + intent | — |
| `SUBMITTED` | request + intent + timestamp | intent + timestamp |
| `ACCEPTED_*` | ack + tier | request_id + intent + tier |
| `SETTLED` | settlement proof | request_id + txid + block |
| `REJECTED` | error code | request_id + error code |

**Recovery rule:**
On restart, wallet loads persisted state and resumes from last known state.
