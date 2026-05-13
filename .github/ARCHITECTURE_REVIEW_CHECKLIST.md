# Architecture Review Checklist

**Purpose**: Enforce architectural invariants for protocol-affecting changes

**Required for**: All PRs touching consensus, state representation, privacy, or off-chain protocols

**Reference**: [docs/architecture/layered_feature_compatibility.md](../docs/architecture/layered_feature_compatibility.md)

---

## Mandatory Checks

### 1. Consensus Layer Changes

**Does this PR change consensus rules, script validation, or spending conditions?**

- [ ] Changes are limited to Layer 0 (Consensus)
- [ ] Does NOT allow proofs/snapshots to substitute for validation
- [ ] Does NOT bypass script evaluation
- [ ] Maintains independent verifiability

**If YES**: Document the consensus change with a BIP-style specification

---

### 2. State Representation Changes

**Does this PR modify UTXO storage, chainstate, or state proofs (e.g., Utreexo)?**

- [ ] Changes affect only HOW state is represented, not WHAT is valid
- [ ] Full validation paths are preserved
- [ ] Proofs are treated as untrusted inputs
- [ ] Does NOT shortcut script execution

**If YES**: Verify that higher layers cannot assume validity from representation alone

---

### 3. Privacy Layer Changes

**Does this PR introduce or modify zero-knowledge proofs, confidential transactions, or privacy mechanisms?**

- [ ] Proofs are **additive** (add privacy), not **substitutive** (replace validation)
- [ ] Consensus still validates concrete, non-ZK data
- [ ] ZK does NOT hide covenant-enforced data
- [ ] ZK is NOT required for consensus validation
- [ ] Privacy is OPTIONAL for all participants

**If YES**: Explicitly document what consensus data remains visible

---

### 4. Off-Chain Protocol Changes

**Does this PR modify Lightning, payment channels, or other Layer 2/3 protocols?**

- [ ] Off-chain logic does NOT trust snapshot state
- [ ] Channel operations use L1 consensus as the sole authority
- [ ] Does NOT create dependencies on Utreexo proofs
- [ ] Maintains contractual anchoring to L1

**If YES**: Verify that dispute resolution relies only on L1 consensus

---

## Layer Boundary Enforcement

### Critical Invariants

Check that this PR does NOT violate:

- [ ] **Lower layers trusting higher layers**
  - Example violation: Consensus trusting a ZK proof without underlying validation

- [ ] **Higher layers weakening lower layers**
  - Example violation: Wallet optimization bypassing script checks

- [ ] **Implicit coupling between orthogonal features**
  - Example violation: Lightning assuming Utreexo proof validity

---

## Prohibited Patterns

Reject PRs that exhibit these anti-patterns:

- [ ] ❌ "ZK proves the covenant was obeyed" → Covenant bypass
- [ ] ❌ "Utreexo implies UTXO validity" → Consensus shortcut
- [ ] ❌ "Lightning trusts snapshot state" → Channel theft risk
- [ ] ❌ "Wallet skips validation because snapshot exists" → Silent corruption

---

## Feature Interaction Matrix

If this PR touches multiple layers, verify:

| Feature A | Feature B | Interaction Type | Valid? |
|-----------|-----------|------------------|--------|
| Taproot   | Utreexo   | Independent      | ✅     |
| Taproot   | ZK        | ZK must not hide covenant data | ⚠️ |
| Utreexo   | Lightning | Independent      | ✅     |
| ZK        | Lightning | Independent unless explicitly extended | ✅ |

---

## Reviewer Questions

Answer these before approving:

1. **What layer does this change primarily affect?**
   - Layer 0 (Consensus), Layer 1 (State), Layer 2 (Privacy), Layer 3 (Off-chain), Layer 4 (UX)

2. **Can consensus validation still work if this feature is disabled?**
   - If NO: This is a consensus dependency (requires extreme scrutiny)
   - If YES: This is properly layered

3. **Does this create any new trust assumptions?**
   - If YES: Document them explicitly and verify they don't violate invariants

4. **Could this enable a shortcut that bypasses fundamental validation?**
   - If YES: Reject the PR

---

## Approval Criteria

**Approve ONLY if**:
- All applicable checkboxes are marked
- No prohibited patterns detected
- Layer boundaries are respected
- No new implicit trust assumptions

**Escalate to architecture discussion if**:
- Unsure about layer classification
- Potential invariant violation
- New feature interaction not covered above

---

## Additional Resources

- [Layered Feature Compatibility](../docs/architecture/layered_feature_compatibility.md) - Full specification
- [Architecture Index](../docs/architecture/README.md) - Normative documents
- [Contributing Guide](../docs/CONTRIBUTING.md) - Coding standards

---

**Remember**: Architecture drift happens through a thousand small compromises. When in doubt, enforce the boundary.
