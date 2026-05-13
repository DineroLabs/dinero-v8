# DPI Error Catalog (ERR_*)

## Version 0.1

---

## Design Principles

1. **Every rejection has a code** — no silent failures
2. **Codes map to threats** — traceability from threat model
3. **Actions are explicit** — what sender/receiver should do
4. **Codes are stable** — never reuse, never renumber

---

## Error Code Format

```
ERR_<LAYER>_<NUMBER>_<SHORT_NAME>

Layer prefixes:
  REQ  = PaymentRequest validation
  INT  = PaymentIntent validation
  UTX  = Utreexo proof validation
  CT   = Confidential Transaction validation
  NET  = Network/mempool issues
  POL  = Policy violations
  SYS  = System/operational errors
```

---

## Layer: PaymentRequest (REQ)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_REQ_001_EXPIRED` | Request expired | `now > expires_at + grace` | 1.5 | Sender: request new QR |
| `ERR_REQ_002_MALFORMED` | Invalid format | JSON parse fail, missing fields | — | Sender: report to merchant |
| `ERR_REQ_003_UNSUPPORTED_VERSION` | Version unknown | `version > supported` | — | Sender: update wallet |
| `ERR_REQ_004_INVALID_ADDRESS` | Bad receiver address | Address decode fails | — | Sender: report to merchant |
| `ERR_REQ_005_AMOUNT_ZERO` | Zero amount | `amount == 0` | — | Sender: report to merchant |
| `ERR_REQ_006_DUPLICATE_REQUEST_ID` | Already fulfilled | `request_id` seen before | 1.1 | Sender: do not pay again |

```json
// Example rejection:
{
  "type": "dinero.payment_reject",
  "error_code": "ERR_REQ_001_EXPIRED",
  "message": "PaymentRequest expired at 1700001234",
  "request_id": "550e8400-e29b-41d4-a716-446655440000"
}
```

---

## Layer: PaymentIntent (INT)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_INT_001_MISSING_TX` | No transaction | `transaction` field empty | — | Sender: rebuild intent |
| `ERR_INT_002_MISSING_PROOF` | No Utreexo proof | `utreexo_proof` field empty | — | Sender: rebuild intent |
| `ERR_INT_003_MISSING_BLINDING` | No blinding factor | CT output without blinding factor | 1.6 | Sender: include blinding factors |
| `ERR_INT_004_REQUEST_MISMATCH` | Wrong request_id | Intent doesn't match request | — | Sender: rebuild for correct request |
| `ERR_INT_005_SIGNATURE_INVALID` | Bad signature | Schnorr verification fails | — | Sender: re-sign transaction |
| `ERR_INT_006_OUTPUT_INDEX_INVALID` | Bad output index | `receiver_output_index` out of bounds | 1.7 | Sender: fix output mapping |
| `ERR_INT_007_WRONG_RECIPIENT` | Output not to receiver | `tx.outputs[idx].address != receiver` | 1.7 | Sender: rebuild transaction |
| `ERR_INT_008_RBF_FORBIDDEN` | RBF signal detected | Transaction signals replaceability | 3.2 | Sender: rebuild without RBF flag |

---

## Layer: Utreexo (UTX)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_UTX_001_PROOF_INVALID` | Proof verification failed | Merkle path doesn't verify | 2.2 | Sender: regenerate proof |
| `ERR_UTX_002_ROOT_UNKNOWN` | Unknown root | Proof root not in receiver's chain | 1.4 | Sender: sync headers, retry |
| `ERR_UTX_003_ROOT_TOO_STALE` | Root too old | `proof.height < current - tolerance` | 1.4 | Sender: sync to recent root |
| `ERR_UTX_004_UTXO_NOT_FOUND` | UTXO doesn't exist | Proof valid but UTXO not in set | — | Sender: input already spent |
| `ERR_UTX_005_PROOF_MALFORMED` | Can't parse proof | Deserialization fails | — | Sender: regenerate proof |

```
Receiver policy example:
  root_height_tolerance: 6  // accept proofs within 6 blocks of tip

if proof.root_height < (current_height - 6):
  return ERR_UTX_003_ROOT_TOO_STALE
```

---

## Layer: Confidential Transactions (CT)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_CT_001_COMMITMENT_MISMATCH` | Amount verification failed | `Pedersen(amount, bf) != commitment` | 1.6 | Sender: correct blinding factor |
| `ERR_CT_002_RANGE_PROOF_INVALID` | Invalid range proof | Range proof verification fails | 2.5 | Sender: regenerate range proof |
| `ERR_CT_003_RANGE_PROOF_MISSING` | No range proof | CT output without range proof | 2.5 | Sender: include range proof |
| `ERR_CT_004_BLINDING_FACTOR_INVALID` | Bad blinding factor | Can't decode blinding factor | 1.6 | Sender: regenerate |

```
Verification pseudocode:
  expected_commitment = pedersen_commit(
    request.amount_una,
    intent.blinding_factors[receiver_output_index]
  )

  if tx.outputs[idx].commitment != expected_commitment:
    return ERR_CT_001_COMMITMENT_MISMATCH
```

---

## Layer: Network (NET)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_NET_001_BROADCAST_FAILED` | Broadcast rejected | No peers accepted tx | 3.4 | Sender: retry, check connectivity |
| `ERR_NET_002_CONFLICT_DETECTED` | Double-spend attempt | Conflicting tx in mempool | 1.3 | Receiver: reject payment |
| `ERR_NET_003_INSUFFICIENT_PROPAGATION` | Not enough peers | `ack_count < min_peers` | 3.1 | Receiver: wait or reject |
| `ERR_NET_004_MEMPOOL_REJECTED` | Mempool policy reject | Fee too low, non-standard, etc. | — | Sender: adjust fee/format |
| `ERR_NET_005_TIMEOUT` | Observation timeout | Tier 2 window expired without resolution | — | Receiver: policy decision |
| `ERR_NET_006_REORG_DETECTED` | Reorg eliminated tx | Confirmed tx no longer in best chain | — | Both: handle reorg |

```
Tier 2 verification:
  broadcast(tx)
  wait(conflict_window)  // e.g., 5 seconds

  if conflict_detected:
    return ERR_NET_002_CONFLICT_DETECTED

  if peer_acks < min_peers:
    return ERR_NET_003_INSUFFICIENT_PROPAGATION

  return TIER_2_ACCEPTED
```

---

## Layer: Policy (POL)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_POL_001_AMOUNT_EXCEEDS_TIER` | Amount too high for tier | Amount > tier threshold | 1.3 | Sender: wait for higher tier |
| `ERR_POL_002_CONFIRMATIONS_REQUIRED` | Needs block confirmation | Policy requires Tier 3 | — | Sender: wait for confirmation |
| `ERR_POL_003_MERCHANT_SUSPENDED` | Merchant not accepting | Receiver temporarily offline | — | Sender: try later |
| `ERR_POL_004_VELOCITY_LIMIT` | Too many payments | Rate limiting triggered | — | Sender: slow down |
| `ERR_POL_005_AMOUNT_BELOW_DUST` | Amount too small | Below dust threshold | — | Sender: increase amount |

```
Merchant policy example:
  tier_2_max_una: 100_000_000    // 1 DIN max at Tier 2
  tier_3_min_confirmations: 1

  if amount > tier_2_max_una && confirmations < 1:
    return ERR_POL_001_AMOUNT_EXCEEDS_TIER
```

---

## Layer: System (SYS)

| Code | Name | Trigger | Threat | Action |
|------|------|---------|--------|--------|
| `ERR_SYS_001_INTERNAL` | Internal error | Unexpected failure | — | Log, alert, investigate |
| `ERR_SYS_002_STORAGE_FULL` | Can't persist state | Disk full | 4.3 | User: free space |
| `ERR_SYS_003_KEY_ACCESS_DENIED` | Can't access keys | Secure enclave locked | 4.1 | User: authenticate |
| `ERR_SYS_004_CLOCK_SKEW` | Clock unreliable | System time far from network time | 1.5 | User: sync clock |
| `ERR_SYS_005_NETWORK_UNAVAILABLE` | No connectivity | Can't reach peers | 3.1 | User: restore connection |

---

## Error Response Format

All DPI rejections use this structure:

```json
{
  "type": "dinero.payment_reject",
  "version": 1,
  "request_id": "uuid-of-original-request",
  "error_code": "ERR_XXX_NNN_NAME",
  "message": "Human-readable explanation",
  "details": {
    "expected": "...",
    "received": "...",
    "hint": "..."
  },
  "timestamp": 1700001234
}
```

**Rules:**
- `error_code` is machine-readable, MUST match catalog
- `message` is human-readable, MAY be localized
- `details` is optional, provides debugging context
- `timestamp` is receiver's clock at rejection time

---

## Error Code Registry Rules

1. **Never reuse codes** — deprecated codes stay reserved forever
2. **Never renumber** — `ERR_REQ_001` is always `EXPIRED`
3. **New codes append** — next REQ error is `ERR_REQ_007_*`
4. **Layers are fixed** — no new layer prefixes without spec revision

---

## Threat → Error Mapping

| Threat ID | Threat Name | Error Code(s) |
|-----------|-------------|---------------|
| 1.1 | PaymentRequest Replay | `ERR_REQ_006_DUPLICATE_REQUEST_ID` |
| 1.3 | Race (Two Receivers) | `ERR_NET_002_CONFLICT_DETECTED` |
| 1.4 | Root Staleness | `ERR_UTX_002_ROOT_UNKNOWN`, `ERR_UTX_003_ROOT_TOO_STALE` |
| 1.5 | Clock Skew | `ERR_REQ_001_EXPIRED`, `ERR_SYS_004_CLOCK_SKEW` |
| 1.6 | CT Amount Mismatch | `ERR_CT_001_COMMITMENT_MISMATCH`, `ERR_INT_003_MISSING_BLINDING` |
| 1.7 | Output Index Confusion | `ERR_INT_006_OUTPUT_INDEX_INVALID`, `ERR_INT_007_WRONG_RECIPIENT` |
| 2.2 | Proof Forgery | `ERR_UTX_001_PROOF_INVALID` |
| 2.5 | Range Proof Invalid | `ERR_CT_002_RANGE_PROOF_INVALID` |
| 3.1 | Mempool Eclipse | `ERR_NET_003_INSUFFICIENT_PROPAGATION` |
| 3.2 | RBF Front-running | `ERR_INT_008_RBF_FORBIDDEN` |
| 3.4 | Miner Censorship | `ERR_NET_001_BROADCAST_FAILED` |

---

## Complete Error Code Summary

| Code | Layer | Description |
|------|-------|-------------|
| `ERR_REQ_001_EXPIRED` | REQ | Request expired |
| `ERR_REQ_002_MALFORMED` | REQ | Invalid JSON/format |
| `ERR_REQ_003_UNSUPPORTED_VERSION` | REQ | Unknown version |
| `ERR_REQ_004_INVALID_ADDRESS` | REQ | Bad receiver address |
| `ERR_REQ_005_AMOUNT_ZERO` | REQ | Zero amount |
| `ERR_REQ_006_DUPLICATE_REQUEST_ID` | REQ | Already fulfilled |
| `ERR_INT_001_MISSING_TX` | INT | No transaction |
| `ERR_INT_002_MISSING_PROOF` | INT | No Utreexo proof |
| `ERR_INT_003_MISSING_BLINDING` | INT | No blinding factor for CT |
| `ERR_INT_004_REQUEST_MISMATCH` | INT | Wrong request_id |
| `ERR_INT_005_SIGNATURE_INVALID` | INT | Bad signature |
| `ERR_INT_006_OUTPUT_INDEX_INVALID` | INT | Index out of bounds |
| `ERR_INT_007_WRONG_RECIPIENT` | INT | Output not to receiver |
| `ERR_INT_008_RBF_FORBIDDEN` | INT | RBF signal detected |
| `ERR_UTX_001_PROOF_INVALID` | UTX | Proof doesn't verify |
| `ERR_UTX_002_ROOT_UNKNOWN` | UTX | Unknown root |
| `ERR_UTX_003_ROOT_TOO_STALE` | UTX | Root too old |
| `ERR_UTX_004_UTXO_NOT_FOUND` | UTX | Input doesn't exist |
| `ERR_UTX_005_PROOF_MALFORMED` | UTX | Can't parse proof |
| `ERR_CT_001_COMMITMENT_MISMATCH` | CT | Amount verification failed |
| `ERR_CT_002_RANGE_PROOF_INVALID` | CT | Bad range proof |
| `ERR_CT_003_RANGE_PROOF_MISSING` | CT | No range proof |
| `ERR_CT_004_BLINDING_FACTOR_INVALID` | CT | Bad blinding factor |
| `ERR_NET_001_BROADCAST_FAILED` | NET | Can't broadcast |
| `ERR_NET_002_CONFLICT_DETECTED` | NET | Double-spend attempt |
| `ERR_NET_003_INSUFFICIENT_PROPAGATION` | NET | Not enough peers |
| `ERR_NET_004_MEMPOOL_REJECTED` | NET | Mempool policy reject |
| `ERR_NET_005_TIMEOUT` | NET | Observation timeout |
| `ERR_NET_006_REORG_DETECTED` | NET | Reorg eliminated tx |
| `ERR_POL_001_AMOUNT_EXCEEDS_TIER` | POL | Amount too high for tier |
| `ERR_POL_002_CONFIRMATIONS_REQUIRED` | POL | Needs block confirmation |
| `ERR_POL_003_MERCHANT_SUSPENDED` | POL | Receiver offline |
| `ERR_POL_004_VELOCITY_LIMIT` | POL | Rate limited |
| `ERR_POL_005_AMOUNT_BELOW_DUST` | POL | Below dust threshold |
| `ERR_SYS_001_INTERNAL` | SYS | Internal error |
| `ERR_SYS_002_STORAGE_FULL` | SYS | Disk full |
| `ERR_SYS_003_KEY_ACCESS_DENIED` | SYS | Secure enclave locked |
| `ERR_SYS_004_CLOCK_SKEW` | SYS | Bad clock |
| `ERR_SYS_005_NETWORK_UNAVAILABLE` | SYS | No connectivity |

---

**Total: 34 error codes across 6 layers**
