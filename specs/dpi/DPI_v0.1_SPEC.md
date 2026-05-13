# Dinero Payment Integration (DPI)
## Specification v0.1

```
Status:         Draft
Version:        0.1.0
Last Updated:   2026-01-23
Authors:        DineroCoin Core Team
```

---

## Table of Contents

1. [Overview](#1-overview)
2. [Terminology](#2-terminology)
3. [Design Principles](#3-design-principles)
4. [Roles and Symmetry](#4-roles-and-symmetry)
5. [Trust Model](#5-trust-model)
6. [Utreexo Root Provenance](#6-utreexo-root-provenance)
7. [Confidential Transaction Binding](#7-confidential-transaction-binding)
8. [Message Formats](#8-message-formats)
9. [Encoding and Transport](#9-encoding-and-transport)
10. [Verification Pipeline](#10-verification-pipeline)
11. [Payment State Machine](#11-payment-state-machine)
12. [Tier Semantics](#12-tier-semantics)
13. [Network Rules](#13-network-rules)
14. [Time and Expiry](#14-time-and-expiry)
15. [Reorg Handling](#15-reorg-handling)
16. [Error Catalog](#16-error-catalog)
17. [Persistence and Recovery](#17-persistence-and-recovery)
18. [Privacy Considerations](#18-privacy-considerations)
19. [Security Considerations](#19-security-considerations)
20. [Conformance Requirements](#20-conformance-requirements)
21. [Versioning](#21-versioning)
22. [Test Vectors](#22-test-vectors)
23. [References](#23-references)

---

## 1. Overview

Dinero Payment Integration (DPI) defines a wallet-level payment protocol enabling:

- **Instant acceptance** with cryptographic correctness
- **Configurable finality** via tier-based confirmation
- **Symmetric wallets** where any wallet can pay or receive
- **No trusted third parties** — verification is local and deterministic

### 1.1 Scope

DPI specifies:
- Message formats for payment requests and intents
- Verification procedures for receivers
- State machine for payment lifecycle
- Error semantics for all failure modes

### 1.2 Non-Goals

DPI does NOT specify:
- Dinero consensus rules
- Block/transaction format
- Global identity registries
- Merchant application UX
- Wallet key management

---

## 2. Terminology

### 2.1 Currency

| Term | Definition |
|------|------------|
| **Dinero (DIN)** | Primary currency unit |
| **una** | Smallest indivisible unit (1 DIN = 10^8 una) |

### 2.2 Roles

| Term | Definition |
|------|------------|
| **Wallet** | Software capable of both sending and receiving payments |
| **Sender** | Wallet acting as payer for a specific transaction |
| **Receiver** | Wallet acting as payee for a specific transaction |

### 2.3 Messages

| Term | Definition |
|------|------------|
| **PaymentRequest** | Receiver-originated message requesting payment |
| **PaymentIntent** | Sender-originated message containing transaction and proofs |
| **PaymentAck** | Receiver-originated acceptance confirmation |
| **PaymentReject** | Receiver-originated rejection with error code |

### 2.4 Tiers

| Term | Definition |
|------|------------|
| **Tier 0** | Human intent captured (QR scanned, request reviewed) |
| **Tier 1** | Cryptographic verification passed |
| **Tier 2** | Network visibility confirmed (no conflicts observed) |
| **Tier 3** | Consensus settlement (block confirmation) |

### 2.5 RFC 2119 Keywords

The keywords MUST, MUST NOT, REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD NOT, RECOMMENDED, MAY, and OPTIONAL are interpreted as described in [RFC 2119].

---

## 3. Design Principles

### 3.1 Wallet Symmetry

```
Principle: There are no "payer wallets" or "payee wallets."
           There are only wallets, with temporary roles per transaction.
```

Every DPI-conformant wallet MUST be capable of:
- Generating PaymentRequests (receiver role)
- Building PaymentIntents (sender role)
- Verifying PaymentIntents (receiver role)
- Processing PaymentAck/Reject (sender role)

### 3.2 Determinism

```
Principle: Same inputs MUST produce same outputs.
           Verification is a pure function.
```

### 3.3 Explicit Failure

```
Principle: Every rejection has a code.
           No silent failures. No ambiguous states.
```

### 3.4 Separation of Concerns

```
Principle: Protocol defines correctness.
           Policy defines acceptance thresholds.
           Implementation defines UX.
```

---

## 4. Roles and Symmetry

### 4.1 Role Assignment

Roles are assigned per-transaction, not per-device:

```
Transaction 1:
  Wallet A (Sender) ──pays──▶ Wallet B (Receiver)

Transaction 2:
  Wallet B (Sender) ──pays──▶ Wallet A (Receiver)
```

### 4.2 Role Transitions

A wallet transitions between roles based on user action:

| User Action | Resulting Role |
|-------------|----------------|
| Generate QR / request payment | Receiver |
| Scan QR / initiate payment | Sender |

### 4.3 Simultaneous Roles

A wallet MAY act as sender and receiver for different transactions concurrently.

A wallet MUST NOT act as both sender and receiver for the same `request_id`.

---

## 5. Trust Model

### 5.1 Assumptions

DPI assumes:
- No trusted intermediaries required
- Network may be adversarial (eclipse, delay, partition)
- Observers are honest-but-curious
- Cryptographic primitives are secure

### 5.2 Trust Anchors

| Anchor | Provides |
|--------|----------|
| Block headers (PoW + chainwork) | Canonical chain identification |
| Schnorr signatures | Spend authorization |
| Utreexo proofs | UTXO existence and non-spending |
| Pedersen commitments + range proofs | Amount validity (CT) |

### 5.3 Trust Boundaries

| Boundary | Trust Transfer |
|----------|----------------|
| Human → Wallet | User trusts UI to encode intent correctly |
| Wallet → Utreexo root | Wallet trusts header chain for current state |
| Wallet → Network | Wallet trusts peer diversity for conflict detection |
| Mempool → Consensus | Mempool acceptance ≠ finality |

---

## 6. Utreexo Root Provenance

### 6.1 Canonical Source

Wallets MUST determine the canonical Utreexo root by:
1. Syncing block headers
2. Validating proof-of-work and chainwork
3. Extracting `utreexo_root` from header at tip (or tip - N for safety margin)

### 6.2 Root Freshness

| Requirement | Value |
|-------------|-------|
| Maximum root staleness (sender) | SHOULD be ≤ 6 blocks |
| Root height tolerance (receiver) | Configurable, default 6 blocks |

### 6.3 Receiver Root Hint

PaymentRequest MAY include a root hint:

```json
{
  "utreexo_root": "0x...",
  "root_height": 850000
}
```

**Rules:**
- Sender MUST NOT trust hint without validating against own headers
- Receiver MUST verify proofs against own canonical root, not provided root
- Hint exists for UX optimization only (reduces sender sync time)

---

## 7. Confidential Transaction Binding

### 7.1 The Problem

With CT, the receiver cannot verify:
```
committed_amount == PaymentRequest.amount
```

Range proofs only prove `0 < amount < 2^64`.

### 7.2 Solution: Blinding Factor Revelation

When PaymentIntent includes CT outputs:

1. Sender MUST include `blinding_factors` mapping output indices to blinding factors
2. Receiver MUST recompute: `C' = Pedersen(request.amount_una, blinding_factor)`
3. Receiver MUST verify: `C' == tx.outputs[idx].commitment`
4. Mismatch → `ERR_CT_001_COMMITMENT_MISMATCH`

### 7.3 Transparent Fallback

Wallets MAY use transparent (non-CT) outputs for DPI payments.

PaymentRequest MAY indicate preference:
```json
{
  "ct_required": false
}
```

---

## 8. Message Formats

All messages use JSON encoding (see §9 for wire format).

### 8.1 PaymentRequest

Generated by receiver, consumed by sender.

```json
{
  "type": "dinero.payment_request",
  "version": 1,
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "receiver_address": "din1qxyz...",
  "amount_una": 125000000,
  "expires_at": 1700001234,
  "memo": "Coffee order #42",
  "utreexo_root": "0xabc...",
  "root_height": 850000,
  "ct_required": true,
  "metadata": {
    "merchant_name": "Coffee House",
    "merchant_id": "coffeehouse.din"
  }
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | REQUIRED | Always `"dinero.payment_request"` |
| `version` | integer | REQUIRED | Protocol version (1 for v0.1) |
| `request_id` | string | REQUIRED | UUIDv4, unique per request |
| `receiver_address` | string | REQUIRED | Bech32m-encoded receiver address |
| `amount_una` | integer | REQUIRED | Amount in una (≥ 1) |
| `expires_at` | integer | REQUIRED | Unix timestamp (seconds) |
| `memo` | string | OPTIONAL | Human-readable description (≤ 256 chars) |
| `utreexo_root` | string | OPTIONAL | Hex-encoded root hint |
| `root_height` | integer | OPTIONAL | Block height for root hint |
| `ct_required` | boolean | OPTIONAL | Require CT output (default: false) |
| `metadata` | object | OPTIONAL | Additional merchant/context info |

### 8.2 PaymentIntent

Generated by sender, consumed by receiver.

```json
{
  "type": "dinero.payment_intent",
  "version": 1,
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "transaction": "0x...",
  "utreexo_proof": "0x...",
  "proof_root_height": 850000,
  "receiver_output_index": 0,
  "blinding_factors": {
    "0": "0x..."
  }
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | REQUIRED | Always `"dinero.payment_intent"` |
| `version` | integer | REQUIRED | Protocol version |
| `request_id` | string | REQUIRED | Must match PaymentRequest |
| `transaction` | string | REQUIRED | Hex-encoded signed transaction |
| `utreexo_proof` | string | REQUIRED | Hex-encoded Utreexo inclusion proof |
| `proof_root_height` | integer | REQUIRED | Height at which proof is valid |
| `receiver_output_index` | integer | REQUIRED | Index of output paying receiver |
| `blinding_factors` | object | CONDITIONAL | Required if CT outputs used |

### 8.3 PaymentAck

Generated by receiver after Tier 2 acceptance.

```json
{
  "type": "dinero.payment_ack",
  "version": 1,
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "txid": "0x...",
  "tier": 2,
  "timestamp": 1700001240
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | REQUIRED | Always `"dinero.payment_ack"` |
| `version` | integer | REQUIRED | Protocol version |
| `request_id` | string | REQUIRED | Matching request |
| `txid` | string | REQUIRED | Transaction ID |
| `tier` | integer | REQUIRED | Acceptance tier (2 or 3) |
| `timestamp` | integer | REQUIRED | Receiver's acceptance time |

### 8.4 PaymentReject

Generated by receiver on any verification failure.

```json
{
  "type": "dinero.payment_reject",
  "version": 1,
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "error_code": "ERR_CT_001_COMMITMENT_MISMATCH",
  "message": "Amount commitment verification failed",
  "timestamp": 1700001235,
  "details": {
    "expected": "0x...",
    "received": "0x..."
  }
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | REQUIRED | Always `"dinero.payment_reject"` |
| `version` | integer | REQUIRED | Protocol version |
| `request_id` | string | REQUIRED | Matching request |
| `error_code` | string | REQUIRED | Canonical ERR_* code |
| `message` | string | REQUIRED | Human-readable explanation |
| `timestamp` | integer | REQUIRED | Receiver's rejection time |
| `details` | object | OPTIONAL | Debugging context |

---

## 9. Encoding and Transport

### 9.1 Wire Encoding

Messages are encoded as:
1. JSON (canonical, no whitespace for hashing)
2. UTF-8 bytes
3. Optional: compressed with zlib for large messages

### 9.2 QR Encoding

PaymentRequest for QR display:

```
Option A (small requests):
  dinero:<base64url-encoded-json>

Option B (large requests):
  dinero://pay?r=<url-to-fetch-request>
```

**QR Capacity:**
- Version 10 QR (57×57): ~174 bytes binary
- Version 20 QR (97×97): ~858 bytes binary
- For larger requests, use URL indirection

### 9.3 Transport

DPI is transport-agnostic. Implementations MAY use:
- Direct device-to-device (NFC, Bluetooth, local network)
- Relay server (sender pushes, receiver polls)
- WebSocket (real-time bidirectional)

**Requirements:**
- Transport SHOULD provide confidentiality (TLS or equivalent)
- Transport MUST deliver messages intact (no partial delivery)
- Transport failures are NOT DPI errors (retry at transport layer)

---

## 10. Verification Pipeline

### 10.1 Pipeline Stages

Receiver MUST execute verification in this exact order:

```
┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐
│  V_REQ  │──▶│  V_INT  │──▶│  V_UTX  │──▶│  V_CT   │──▶│  V_POL  │
└─────────┘   └─────────┘   └─────────┘   └─────────┘   └─────────┘
     │             │             │             │             │
     ▼             ▼             ▼             ▼             ▼
  ERR_REQ_*    ERR_INT_*    ERR_UTX_*    ERR_CT_*     ERR_POL_*
```

### 10.2 Stage Definitions

**V_REQ: Request Validation**
```
- request_id is valid UUIDv4
- request_id not previously fulfilled
- expires_at is in future (with grace period)
- amount_una > 0
- receiver_address is valid
```

**V_INT: Intent Validation**
```
- All required fields present
- request_id matches
- transaction parses correctly
- signature(s) valid
- receiver_output_index in bounds
- output[receiver_output_index] pays receiver_address
- transaction does NOT signal RBF
```

**V_UTX: Utreexo Validation**
```
- Proof parses correctly
- proof_root_height within tolerance of current tip
- Proof verifies against canonical root
- All inputs exist in UTXO set
```

**V_CT: Confidential Transaction Validation**
```
- If CT output: blinding_factor provided
- Recomputed commitment matches transaction commitment
- Range proof valid
```

**V_POL: Policy Validation**
```
- Amount within tier limits
- Velocity limits not exceeded
- Merchant-specific rules satisfied
```

### 10.3 Execution Rules

1. Execute stages sequentially
2. Stop on first failure
3. Return single error code (first failure)
4. On success: proceed to Tier 1 acceptance

### 10.4 Determinism Requirement

```
verify(request, intent, receiver_state) → Result<Accept, Reject>

Given identical inputs, verify() MUST return identical output.
No randomness. No timing dependencies. No external calls during verification.
```

---

## 11. Payment State Machine

### 11.1 States

| State | Terminal | Description |
|-------|----------|-------------|
| `INIT` | No | No payment in progress |
| `REQUESTED` | No | PaymentRequest received by sender |
| `BUILT` | No | PaymentIntent constructed |
| `SUBMITTED` | No | Intent sent to receiver |
| `VERIFYING` | No | Receiver executing pipeline |
| `ACCEPTED_TIER_1` | No | Crypto verification passed |
| `ACCEPTED_TIER_2` | No | Network observation passed |
| `CONFIRMING` | No | Awaiting block confirmations |
| `SETTLED` | **Yes** | Payment finalized |
| `REJECTED` | **Yes** | Payment failed with error |
| `EXPIRED` | **Yes** | Request timed out |

### 11.2 Transitions

```
INIT ──▶ REQUESTED ──▶ BUILT ──▶ SUBMITTED ──▶ VERIFYING
                                                   │
                          ┌────────────────────────┼────────────────┐
                          ▼                        ▼                ▼
                      REJECTED              ACCEPTED_TIER_1     EXPIRED
                                                   │
                          ┌────────────────────────┤
                          ▼                        ▼
                      REJECTED              ACCEPTED_TIER_2
                                                   │
                          ┌────────────────────────┤
                          ▼                        ▼
                      REJECTED               CONFIRMING
                                                   │
                          ┌────────────────────────┤
                          ▼                        ▼
                      REJECTED                SETTLED
```

### 11.3 Invariants

| ID | Invariant |
|----|-----------|
| SM-1 | Payment in exactly one state at any time |
| SM-2 | Terminal states have no outbound transitions |
| SM-3 | States progress monotonically (no backward transitions) |
| SM-4 | Same request_id → same payment (no duplicates) |
| SM-5 | Every rejection includes exactly one ERR_* code |

---

## 12. Tier Semantics

### 12.1 Tier Definitions

| Tier | Name | Proves | Receiver Action |
|------|------|--------|-----------------|
| 0 | Intent | Human meant to pay | Display confirmation UI |
| 1 | Validity | Funds exist, unspent, spendable | May acknowledge internally |
| 2 | Visibility | No network conflicts observed | Send PaymentAck, may release goods |
| 3 | Settlement | Block confirmed | Final accounting |

### 12.2 Tier Progression

```
Tier 0 ──▶ Tier 1 ──▶ Tier 2 ──▶ Tier 3
 (UX)     (crypto)   (network)  (consensus)
```

Tiers are cumulative. Tier 3 implies Tier 2 implies Tier 1.

### 12.3 Acceptance Policy (Receiver-Defined)

Receivers define acceptance policy per amount tier:

```json
{
  "acceptance_policy": {
    "micropayment": {
      "max_una": 10000000,
      "accept_at": "tier_1"
    },
    "retail": {
      "max_una": 1000000000,
      "accept_at": "tier_2",
      "observation_window_ms": 5000,
      "min_peer_acks": 8
    },
    "high_value": {
      "min_una": 1000000001,
      "accept_at": "tier_3",
      "min_confirmations": 3
    }
  }
}
```

### 12.4 Tier 2 Requirements

Tier 2 acceptance requires ALL of:
- Transaction broadcast succeeds
- No conflicting transaction observed within `observation_window`
- At least `min_peer_acks` peers acknowledge receipt
- Transaction does NOT signal RBF

---

## 13. Network Rules

### 13.1 RBF Prohibition

Transactions signaling Replace-By-Fee MUST NOT be accepted at Tier 2.

**Detection:**
- Check sequence numbers for RBF signal
- If `sequence < 0xFFFFFFFE` on any input → signals RBF

**Response:**
- Return `ERR_INT_008_RBF_FORBIDDEN`

### 13.2 Broadcast Requirements

Receiver MUST:
- Broadcast transaction to at least N peers (configurable, default 8)
- Use diverse peer connections (multiple ASNs where possible)
- Track peer acknowledgements

### 13.3 Conflict Detection

During observation window, receiver MUST:
- Monitor mempool for transactions spending same inputs
- If conflict detected → `ERR_NET_002_CONFLICT_DETECTED`

### 13.4 Mempool Non-Persistence

DPI MUST NOT assume mempool persistence.

Tier 2 acceptance means: "no conflict observed during window"
NOT: "transaction will remain in mempool forever"

---

## 14. Time and Expiry

### 14.1 Clock Source

- PaymentRequest uses receiver's clock for `expires_at`
- Sender evaluates against sender's clock
- Receiver evaluates against receiver's clock

### 14.2 Expiry Evaluation

**Sender:**
```
if now_sender > request.expires_at - SENDER_BUFFER:
    reject locally (do not submit)

SENDER_BUFFER = 30 seconds (recommended)
```

**Receiver:**
```
if now_receiver > request.expires_at + RECEIVER_GRACE:
    return ERR_REQ_001_EXPIRED

RECEIVER_GRACE = 60 seconds (recommended)
```

### 14.3 Recommended Expiry Windows

| Context | Expiry |
|---------|--------|
| In-person retail | 5 minutes |
| Online checkout | 15 minutes |
| Invoice | 24 hours |

---

## 15. Reorg Handling

### 15.1 During CONFIRMING State

If confirmed transaction is removed from best chain:

```
if tx in mempool:
    // Survived reorg, re-confirm
    reset confirmation count to 0
    remain in CONFIRMING
else:
    // Lost to competing transaction
    transition to REJECTED
    error = ERR_NET_006_REORG_DETECTED
```

### 15.2 After SETTLED

Reorgs deeper than settlement depth are outside DPI scope.

Recommended settlement depths:
| Amount | Confirmations |
|--------|---------------|
| < 100 DIN | 1 |
| 100-1000 DIN | 3 |
| > 1000 DIN | 6 |

---

## 16. Error Catalog

### 16.1 Error Code Format

```
ERR_<LAYER>_<NUMBER>_<NAME>

Layers: REQ, INT, UTX, CT, NET, POL, SYS
```

### 16.2 Complete Catalog

See [DPI_ERROR_CATALOG.md](./DPI_ERROR_CATALOG.md) for the full error catalog.

### 16.3 Registry Rules

1. Codes are permanent — never reuse or renumber
2. New codes append with next available number
3. Deprecated codes remain reserved

---

## 17. Persistence and Recovery

### 17.1 Minimum Persistence

| State | Sender Persists | Receiver Persists |
|-------|-----------------|-------------------|
| REQUESTED | request | — |
| BUILT | request, intent | — |
| SUBMITTED | request, intent, submit_time | intent, receive_time |
| ACCEPTED_* | request, intent, ack | request_id, intent, tier, ack_time |
| SETTLED | request, intent, ack, block | request_id, txid, block |
| REJECTED | request, intent, error | request_id, error |

### 17.2 Recovery Behavior

On restart:
1. Load persisted payment states
2. For each non-terminal payment:
   - Re-evaluate current state
   - Resume from appropriate point
3. Check blockchain for any pending transactions

---

## 18. Privacy Considerations

### 18.1 Information Exposure

| Entity | Learns |
|--------|--------|
| Sender | Receiver address, amount, merchant metadata |
| Receiver | Sender's UTXO (via proof), amount, txid |
| Network observers | Transaction graph, timing (amounts hidden if CT) |
| Transport relay | Message sizes, timing (content encrypted if TLS) |

### 18.2 Mitigations

- Use CT outputs to hide amounts from chain observers
- Use fresh addresses per PaymentRequest
- Use Tor/I2P for network layer privacy
- Minimize metadata in PaymentRequest

### 18.3 Blinding Factor Security

Blinding factors:
- MUST be generated with CSPRNG
- MUST NOT be reused across transactions
- Are revealed only to intended receiver

---

## 19. Security Considerations

### 19.1 Threats Addressed

| Threat | Mitigation |
|--------|------------|
| PaymentRequest replay | Unique request_id, expiry, duplicate tracking |
| Double-spend race | Tier 2 observation window, conflict detection |
| Utreexo root staleness | Height tolerance, header validation |
| CT amount manipulation | Commitment verification via blinding factor |
| Mempool eclipse | Peer diversity, minimum ack threshold |
| RBF front-running | RBF prohibition at Tier 2 |
| QR spoofing | Merchant identity display, user verification |

### 19.2 Residual Risks

| Risk | Mitigation | Residual |
|------|------------|----------|
| Sophisticated eclipse | Peer diversity | ISP-level attacker can still eclipse |
| Key compromise | Secure enclave | Physical access defeats all |
| Reorg attack | Confirmation depth | Deep reorg outside scope |

### 19.3 Implementation Guidance

Implementations MUST:
- Use constant-time comparison for signatures and proofs
- Validate all inputs before processing
- Fail closed (reject on any anomaly)
- Log all state transitions for audit

---

## 20. Conformance Requirements

### 20.1 Conformance Levels

**DPI v0.1 Core Conformant:**
- Implements full state machine
- Enforces verification pipeline order
- Returns correct ERR_* codes
- Passes all reference test vectors
- Handles malformed input without panic

**DPI v0.1 Full Conformant:**
- Core Conformant, plus:
- Supports CT amount binding
- Implements Tier 2 network observation
- Supports PaymentRequest QR encoding

### 20.2 Test Vector Compliance

Implementations MUST pass all test vectors defined in §22.

### 20.3 Interoperability

Two conformant implementations MUST:
- Successfully complete happy-path payments
- Agree on acceptance/rejection for all test vectors
- Produce identical error codes for identical failure conditions

---

## 21. Versioning

### 21.1 Version Field

All messages include `version` field.

Current version: `1`

### 21.2 Compatibility Rules

| Change Type | Version Impact |
|-------------|----------------|
| Add optional field | Minor (compatible) |
| Add required field | Major (breaking) |
| Remove field | Major (breaking) |
| Change field semantics | Major (breaking) |
| Add error code | Minor (compatible) |

### 21.3 Version Negotiation

- Sender uses version from PaymentRequest
- Unknown version → `ERR_REQ_003_UNSUPPORTED_VERSION`
- Future versions MAY include version negotiation

---

## 22. Test Vectors

See [../tests/dpi/](../../tests/dpi/) for complete test vectors.

### 22.1 Required Vectors

| ID | Description | Tests |
|----|-------------|-------|
| TV_001 | Happy path (transparent) | Full pipeline |
| TV_002 | Happy path (CT) | CT binding |
| TV_003 | Expired request | ERR_REQ_001 |
| TV_004 | Duplicate request_id | ERR_REQ_006 |
| TV_005 | Invalid signature | ERR_INT_005 |
| TV_006 | Wrong recipient | ERR_INT_007 |
| TV_007 | RBF flagged | ERR_INT_008 |
| TV_008 | Stale Utreexo root | ERR_UTX_003 |
| TV_009 | Invalid Utreexo proof | ERR_UTX_001 |
| TV_010 | CT commitment mismatch | ERR_CT_001 |
| TV_011 | Invalid range proof | ERR_CT_002 |
| TV_012 | Amount exceeds tier | ERR_POL_001 |
| TV_013 | Malformed JSON | ERR_REQ_002 |
| TV_014 | Missing required field | ERR_INT_001 |

---

## 23. References

- [RFC 2119] Key words for use in RFCs
- [BIP340] Schnorr Signatures for secp256k1
- [Utreexo] Utreexo: A dynamic hash-based accumulator
- [Pedersen] Pedersen Commitments
- [CT] Confidential Transactions

---

## Appendix A: Quick Reference

### A.1 Message Type Summary

| Message | Direction | Purpose |
|---------|-----------|---------|
| PaymentRequest | Receiver → Sender | Request payment |
| PaymentIntent | Sender → Receiver | Submit payment |
| PaymentAck | Receiver → Sender | Confirm acceptance |
| PaymentReject | Receiver → Sender | Report failure |

### A.2 State Transition Quick Reference

```
INIT → REQUESTED → BUILT → SUBMITTED → VERIFYING
                                          ↓
                         ACCEPTED_TIER_1 → ACCEPTED_TIER_2 → CONFIRMING → SETTLED
                                ↓                  ↓              ↓
                             REJECTED          REJECTED       REJECTED
```

### A.3 Verification Order

```
V_REQ → V_INT → V_UTX → V_CT → V_POL → ACCEPT
```

---

## Appendix B: Implementation Checklist

```
[ ] PaymentRequest generation
[ ] PaymentRequest QR encoding
[ ] PaymentRequest parsing
[ ] PaymentIntent construction
[ ] Utreexo proof generation
[ ] Blinding factor handling (CT)
[ ] PaymentIntent serialization
[ ] V_REQ implementation
[ ] V_INT implementation
[ ] V_UTX implementation
[ ] V_CT implementation
[ ] V_POL implementation
[ ] State machine implementation
[ ] Error code mapping
[ ] Persistence layer
[ ] Recovery logic
[ ] Test vector compliance
```
