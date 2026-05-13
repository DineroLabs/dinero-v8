# Layered Feature Compatibility & Invariants

**Status**: Canonical / Normative
**Version**: 1.0
**Last Updated**: 2025-12-24

---

## Executive Summary

The DineroCoin system may include **Taproot Covenants**, **Utreexo**, **Zero-Knowledge techniques**, **Lightning**, and **AssumeUTXO** simultaneously.

**However**:
- They must **not** operate at the same layer
- They must **not** implicitly trust one another
- Consensus must remain **authoritative** and **independently verifiable**

When these constraints are respected, the features are **orthogonal**, not conflicting.

---

## Design Principle

> **Acceleration, compression, privacy, and off-chain protocols must never become authorities.**

Consensus validation must remain possible **without trusting**:
- Snapshots
- Accumulators
- Proofs
- Off-chain state
- Wallet shortcuts

---

## System Dimensions (Not a Single Feature)

| Feature                | Dimension                  | Touches Consensus |
|------------------------|----------------------------|-------------------|
| Taproot + Covenants    | Script / Spending Rules    | ⚠️ Yes            |
| Utreexo                | State Representation       | ⚠️ Yes            |
| Zero-Knowledge (ZK)    | Privacy / Proofs           | ⚠️ Potentially    |
| Lightning              | Off-Chain Protocol         | ❌ No             |

These dimensions are **orthogonal** and only conflict if boundaries are violated.

---

## 1. Taproot + Covenants

### Layer: Consensus (Script Semantics)

#### Affects
- Script evaluation
- Spending conditions
- Mempool and block validity

#### Does Not Affect
- UTXO storage format
- Chainstate representation
- Wallet accounting
- Lightning mechanics

#### Compatibility
- ✅ Compatible with Utreexo
- ✅ Compatible with Lightning
- ⚠️ Dangerous only if ZK hides covenant-relevant data

#### Invariant
**Covenants are enforced at script evaluation time, not via proofs or metadata.**

**Verdict**: ✔️ Foundational and correct

---

## 2. Utreexo

### Layer: State Representation

#### Critical Clarification
**Utreexo does not change what is valid.**
**It changes how validity is represented.**

#### Affects
- UTXO storage
- Proof carriage
- Node resource usage

#### Does Not Affect
- Script semantics
- Taproot logic
- Covenants
- Lightning
- Wallet correctness (post-abstraction)

#### Only Real Risk
⚠️ Allowing higher layers to assume UTXO presence without proof verification

#### Mitigation (Already Implemented)
- Proofs treated as **untrusted**
- Full validation paths **preserved**
- No script shortcutting

**Verdict**: ✔️ Fully compatible

---

## 3. Zero-Knowledge (ZK)

### Layer: Privacy / Optional Proofs

**This is the only area requiring ongoing restraint.**

### ZK Is Safe If and Only If
- Proofs are **additive**, not **substitutive**
- Consensus still validates **concrete data**
- Script enforcement is **never bypassed**

### Safe Use Cases
- ✅ Private balance proofs
- ✅ Confidential transactions
- ✅ Wallet-side validation
- ✅ Off-chain proof systems
- ✅ Layered privacy (CT-style)

### Dangerous Use Cases
- ❌ "Trust me this spend is valid"
- ❌ Hiding covenant-enforced data
- ❌ ZK replacing script execution
- ❌ ZK-only UTXO validity

> **If ZK becomes required for consensus validation, the system has forked into a ZK-chain.**

**Verdict**: ✔️ Acceptable only as an **optional privacy layer**

---

## 4. Lightning

### Layer: Off-Chain Protocol

#### Characteristics
- Off-chain
- Contractually anchored to L1
- Uses Taproot as a **tool**, not a dependency
- Independent of Utreexo
- Independent of ZK unless explicitly extended

#### Requires Only
- Stable script semantics
- Reliable reorg handling
- Correct timelocks

All are already satisfied.

**Verdict**: ✔️ No conflicts

---

## Known Conflict Patterns (Explicitly Forbidden)

| Bad Pattern                                        | Result               |
|----------------------------------------------------|----------------------|
| "ZK proves the covenant was obeyed"                | Covenant bypass      |
| "Utreexo implies UTXO validity"                    | Consensus shortcut   |
| "Lightning trusts snapshot state"                  | Channel theft        |
| "Wallet skips validation because snapshot exists"  | Silent corruption    |

**These are architectural violations, not bugs.**

---

## Canonical Layer Model

| Layer   | Responsibility                        |
|---------|---------------------------------------|
| Layer 0 | Consensus rules (final, boring, slow) |
| Layer 1 | State representation (Utreexo, snapshots) |
| Layer 2 | Privacy (ZK, CT)                      |
| Layer 3 | Off-chain protocols (Lightning)       |
| Layer 4 | Wallet / UX optimizations             |

---

## Mandatory Invariants

1. **Lower layers never trust higher layers**
2. **Higher layers never weaken lower layers**

---

## Final Assessment

| Status | Finding                                         |
|--------|-------------------------------------------------|
| ❌     | No dangerous feature combination exists         |
| ❌     | Nothing needs to be removed                     |
| ⚠️     | ZK requires continued discipline                |
| ✅     | Architecture is internally consistent           |
| ✅     | Timing of this review is exactly right          |

---

## Recommendation

**Treat this document as normative:**
- Refer to it during code reviews
- Reject features that violate it
- Use it to explain the architecture to auditors and contributors

---

## References

- **BIP 340-342**: Taproot specification
- **Utreexo whitepaper**: Dynamic hash-based accumulators
- **Lightning Network specification**: BOLT standards
- **AssumeUTXO**: Bitcoin Core BIP proposal

---

## Modification Policy

**This is a normative document under architecture freeze.**

Changes to this document are **rare and explicit**. See [ARCHITECTURE_FREEZE_POLICY.md](./ARCHITECTURE_FREEZE_POLICY.md) for the change process.

**Summary**:
- **Clarifications** (typos, examples): Standard PR review
- **Extensions** (new layers, patterns): 14-day review + committee approval
- **Breaking changes** (remove/modify invariants): 30-day grace + governance vote

**Goal**: Architectural stability enables contributors to build on long-term guarantees without fear of retroactive invalidation.

---

## Change Log

| Date       | Version | Change                    |
|------------|---------|---------------------------|
| 2025-12-24 | 1.0     | Initial canonical version |
