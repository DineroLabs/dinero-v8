# DPI Threat Model

## Version 0.1

---

## 1. Threat Modeling Approach

**Methodology:** STRIDE-per-element applied to DPI payment flow
**Scope:** Sender wallet, Receiver wallet, Network, and their interfaces
**Assumptions:**
- Cryptographic primitives (Schnorr, Pedersen, Bulletproofs) are secure
- Network is adversarial (Byzantine)
- Devices may be compromised at operational level (not cryptographic)

---

## 2. Trust Boundaries

```
┌─────────────────────────────────────────────────────────────────────┐
│                        UNTRUSTED ZONE                                │
│  ┌─────────────┐    ┌──────────────┐    ┌─────────────────────────┐ │
│  │   Network   │    │   Mempool    │    │    Other Wallets        │ │
│  │  (peers)    │    │  (shared)    │    │    (adversarial)        │ │
│  └──────┬──────┘    └──────┬───────┘    └───────────┬─────────────┘ │
│         │                  │                        │               │
└─────────┼──────────────────┼────────────────────────┼───────────────┘
          │                  │                        │
          │    ┌─────────────┴─────────────┐          │
          │    │      TRUST BOUNDARY       │          │
          │    └─────────────┬─────────────┘          │
          │                  │                        │
┌─────────┼──────────────────┼────────────────────────┼───────────────┐
│         ▼                  ▼                        ▼               │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    LOCAL WALLET                              │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │    │
│  │  │  Key Store   │  │  State DB    │  │  Verifier Logic  │   │    │
│  │  │  (trusted)   │  │  (trusted)   │  │  (deterministic) │   │    │
│  │  └──────────────┘  └──────────────┘  └──────────────────┘   │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                        TRUSTED ZONE                                  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. Threat Categories

### Category 1: Payment Integrity Threats

| ID | Threat | Description | Attacker | Impact |
|----|--------|-------------|----------|--------|
| 1.1 | PaymentRequest Replay | Reuse of fulfilled request_id | Malicious receiver | Double-charge sender |
| 1.2 | PaymentIntent Replay | Resubmit same intent to different receiver | Malicious sender | N/A (bound to receiver) |
| 1.3 | Race Condition | Send same UTXO to two receivers | Malicious sender | One receiver loses funds |
| 1.4 | Utreexo Root Staleness | Accept proof against outdated root | Malicious sender | Spend already-spent UTXO |
| 1.5 | Clock Skew Exploitation | Manipulate expiry checks | Either party | Premature/late acceptance |
| 1.6 | CT Amount Mismatch | Commit to wrong amount | Malicious sender | Underpay receiver |
| 1.7 | Output Index Confusion | Point to wrong output | Malicious sender | Payment to wrong address |

### Category 2: Cryptographic Threats

| ID | Threat | Description | Attacker | Impact |
|----|--------|-------------|----------|--------|
| 2.1 | Signature Forgery | Forge Schnorr signature | External | Steal funds |
| 2.2 | Utreexo Proof Forgery | Forge Merkle inclusion proof | Malicious sender | Accept invalid UTXO |
| 2.3 | Commitment Manipulation | Open commitment to different value | Malicious sender | Underpay receiver |
| 2.4 | Blinding Factor Leak | Derive blinding factor from public data | External | Privacy breach |
| 2.5 | Range Proof Bypass | Accept output without valid range proof | Malicious sender | Inflation attack |

### Category 3: Network Threats

| ID | Threat | Description | Attacker | Impact |
|----|--------|-------------|----------|--------|
| 3.1 | Mempool Eclipse | Isolate receiver from seeing conflicts | Network attacker | Accept double-spend |
| 3.2 | RBF Front-running | Replace tx after Tier 1 acceptance | Malicious sender | Redirect payment |
| 3.3 | Confirmation Delay | Prevent tx from confirming | Miner cartel | Stuck payment |
| 3.4 | Miner Censorship | Refuse to mine specific tx | Miner cartel | Payment failure |
| 3.5 | Sybil Propagation | Fake peer acknowledgments | Network attacker | False Tier 2 confidence |

### Category 4: Operational Threats

| ID | Threat | Description | Attacker | Impact |
|----|--------|-------------|----------|--------|
| 4.1 | Key Extraction | Extract private keys from device | Physical attacker | Steal all funds |
| 4.2 | State Corruption | Modify persisted payment state | Local attacker | Lose payment records |
| 4.3 | Storage Exhaustion | Fill storage to prevent persistence | DoS attacker | State loss |
| 4.4 | UI Spoofing | Display wrong payment details | Malware | User approves wrong payment |

---

## 4. Threat Analysis

### 1.1 PaymentRequest Replay

**Scenario:**
1. Sender pays request_id=ABC for 10 DIN
2. Receiver keeps request_id=ABC active
3. Sender is tricked into paying again

**Mitigation:**
- Receiver MUST reject duplicate request_id (`ERR_REQ_006`)
- Sender wallet SHOULD track paid request_ids locally
- request_id includes randomness (UUID v4)

**Residual Risk:** Low (requires sender UI manipulation)

---

### 1.3 Race Condition (Double-Spend to Two Receivers)

**Scenario:**
1. Sender creates two intents spending same UTXO
2. Sends to Receiver A and Receiver B simultaneously
3. Both accept at Tier 1 (crypto valid)
4. Only one confirms; other loses

**Mitigation:**
- Tier 2 requires network observation window
- Conflict detection (`ERR_NET_002`)
- Receivers observe mempool for conflicts before Tier 2

**Residual Risk:** Medium (sophisticated timing attack possible)

---

### 1.4 Utreexo Root Staleness

**Scenario:**
1. Sender creates proof against old root
2. UTXO was spent after that root
3. Receiver accepts stale proof
4. Payment fails at broadcast

**Mitigation:**
- `root_height_tolerance` policy (e.g., 6 blocks)
- Receiver rejects roots older than tolerance (`ERR_UTX_003`)
- Hybrid root discovery: header sync + receiver hint

**Residual Risk:** Low (configurable tolerance)

---

### 1.5 Clock Skew Exploitation

**Scenario:**
1. Receiver clock is 5 minutes ahead
2. Valid request appears expired
3. Legitimate payment rejected

**Mitigation:**
- Grace period on expiry checks (e.g., 60 seconds)
- System clock monitoring (`ERR_SYS_004`)
- Use network time hints

**Residual Risk:** Low (grace period absorbs typical skew)

---

### 1.6 CT Amount Mismatch

**Scenario:**
1. Request asks for 100 DIN
2. Sender creates commitment for 10 DIN
3. Provides blinding factor for 10 DIN
4. Receiver cannot detect without verification

**Mitigation:**
- Mandatory blinding factor revelation
- Receiver recomputes Pedersen commitment
- Mismatch triggers `ERR_CT_001`

**Residual Risk:** None (cryptographically enforced)

---

### 1.7 Output Index Confusion

**Scenario:**
1. Transaction has multiple outputs
2. Sender specifies wrong `receiver_output_index`
3. Receiver credits wrong amount or address

**Mitigation:**
- Verify output at index matches receiver address (`ERR_INT_007`)
- Verify commitment at index matches amount (`ERR_CT_001`)

**Residual Risk:** None (explicit verification)

---

### 2.2 Utreexo Proof Forgery

**Scenario:**
1. Sender constructs fake Merkle proof
2. Claims UTXO exists that doesn't
3. Receiver accepts phantom UTXO

**Mitigation:**
- Cryptographic proof verification (`ERR_UTX_001`)
- Receiver maintains/validates accumulator root
- Invalid proofs rejected deterministically

**Residual Risk:** None (cryptographically impossible)

---

### 2.5 Range Proof Bypass

**Scenario:**
1. CT output without range proof
2. Output could commit to negative value
3. Inflation attack possible

**Mitigation:**
- Mandatory range proof verification (`ERR_CT_002`, `ERR_CT_003`)
- Bulletproofs verify amount in valid range [0, 2^64)

**Residual Risk:** None (protocol enforced)

---

### 3.1 Mempool Eclipse

**Scenario:**
1. Attacker controls receiver's network view
2. Conflicting tx hidden from receiver
3. Receiver accepts at Tier 2
4. Attacker's tx confirms instead

**Mitigation:**
- Multi-peer observation (`min_peers` threshold)
- Insufficient propagation triggers `ERR_NET_003`
- Diverse peer selection

**Residual Risk:** Medium (sophisticated network attack)

---

### 3.2 RBF Front-running

**Scenario:**
1. Sender submits RBF-enabled tx
2. Receiver accepts at Tier 1
3. Sender broadcasts higher-fee replacement
4. Replacement redirects funds

**Mitigation:**
- RBF signal detection in V_INT
- RBF transactions rejected (`ERR_INT_008`)
- Only final (non-replaceable) tx accepted

**Residual Risk:** None (protocol enforced)

---

### 4.1 Key Extraction

**Scenario:**
1. Physical attacker gains device access
2. Extracts private keys from storage
3. Steals all funds

**Mitigation:**
- Secure enclave/TEE for key storage
- Key access requires authentication (`ERR_SYS_003`)
- Hardware wallet integration (future)

**Residual Risk:** Medium (depends on device security)

---

## 5. Threat-to-Mitigation Matrix

| Threat | V_REQ | V_INT | V_UTX | V_CT | V_POL | Network | Policy |
|--------|-------|-------|-------|------|-------|---------|--------|
| 1.1 Replay | `ERR_REQ_006` | | | | | | |
| 1.3 Race | | | | | | `ERR_NET_002` | Tier 2 window |
| 1.4 Staleness | | | `ERR_UTX_003` | | | | root_tolerance |
| 1.5 Clock | `ERR_REQ_001` | | | | | | grace_period |
| 1.6 Amount | | `ERR_INT_003` | | `ERR_CT_001` | | | |
| 1.7 Index | | `ERR_INT_006/007` | | | | | |
| 2.2 Proof | | | `ERR_UTX_001` | | | | |
| 2.5 Range | | | | `ERR_CT_002/003` | | | |
| 3.1 Eclipse | | | | | | `ERR_NET_003` | min_peers |
| 3.2 RBF | | `ERR_INT_008` | | | | | |

---

## 6. Risk Summary

| Risk Level | Threats | Mitigation Status |
|------------|---------|-------------------|
| **Critical** | None | — |
| **High** | 4.1 (Key extraction) | Device-dependent |
| **Medium** | 1.3 (Race), 3.1 (Eclipse) | Tier 2 observation |
| **Low** | 1.4, 1.5, 1.1 | Protocol mitigations |
| **None** | 1.6, 1.7, 2.2, 2.5, 3.2 | Cryptographic enforcement |

---

## 7. Security Assumptions

1. **Schnorr signatures** are existentially unforgeable under chosen-message attack
2. **Pedersen commitments** are computationally binding and perfectly hiding
3. **Bulletproofs** soundly prove range membership
4. **Utreexo** accumulator is collision-resistant
5. **Network** is partially synchronous (messages eventually deliver)
6. **Receiver** has at least `min_peers` honest connections
7. **Device** secure enclave protects keys at rest

---

## 8. Out of Scope

The following are explicitly outside DPI's threat model:

| Item | Reason |
|------|--------|
| 51% attacks | Blockchain-level, not payment-level |
| Quantum computing | Future protocol revision |
| Side-channel attacks | Device-specific |
| Social engineering | User education |
| Regulatory compliance | Jurisdiction-specific |
| Deep reorgs (>settlement depth) | Configurable policy |

---

## 9. Recommendations

### For Wallet Implementers

1. **Always verify** the full V_* pipeline; never skip steps
2. **Diverse peer connections** to mitigate eclipse attacks
3. **Persist state** before and after critical transitions
4. **Display clear confirmation** UI to prevent spoofing
5. **Use secure enclave** for key material when available

### For Merchants

1. **Configure tier thresholds** appropriate to goods value
2. **Higher-value transactions** should require Tier 3 (confirmation)
3. **Monitor for patterns** suggesting double-spend attempts
4. **Implement velocity limits** to bound exposure

### For Protocol Evolution

1. **Hardware wallet support** for enhanced key security
2. **Payment channels** for high-frequency low-value payments
3. **Watchtower services** for offline conflict detection
