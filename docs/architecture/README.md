# Architecture Documentation

This directory contains **canonical architectural documents** for DineroCoin.

---

## Normative Documents

These documents are **mandatory** reading for all protocol changes:

### **[layered_feature_compatibility.md](./layered_feature_compatibility.md)**

**Status**: Canonical / Normative

Defines the fundamental architectural invariants governing the interaction between:
- Consensus layer (Taproot, Covenants)
- State representation (Utreexo, AssumeUTXO)
- Privacy layer (Zero-Knowledge proofs)
- Off-chain protocols (Lightning)

**All protocol-affecting changes MUST comply with this document.**

### **[ARCHITECTURE_FREEZE_POLICY.md](./ARCHITECTURE_FREEZE_POLICY.md)**

**Status**: Normative

Establishes the process for changing normative architecture documents. The architectural layer is **frozen** — not the code, but the rules. Changes to normative documents are rare, explicit, and follow a review process similar to consensus changes.

**Key Points**:
- **Clarifications**: Easy (typo fixes, examples)
- **Extensions**: Require 14-day review + committee approval
- **Breaking changes**: Require 30-day grace + governance vote
- **Goal**: Architectural drift requires conscious decision, not accident

---

## Architecture Compliance

### For Contributors

Before submitting a PR that touches:
- Consensus rules
- State representation
- Privacy mechanisms
- Off-chain protocols

You **must** review it against the normative documents in this directory.

### For Reviewers

PRs violating the architectural invariants defined here should be **rejected** with a reference to the specific violated invariant.

---

## Reference Documents

Additional architectural documentation:
- [Canonical State And Recovery Plan](./CANONICAL_STATE_AND_RECOVERY_PLAN.md)
- [Shielded State Classification](./SHIELDED_STATE_CLASSIFICATION.md)
- [RPC Migration Guide](./RPC_MIGRATION_GUIDE.md)
- [RPC Quick Reference](./RPC_QUICK_REFERENCE.md)

---

## Questions?

If you're unsure whether a change complies with the architecture:
1. Read the relevant normative document carefully
2. Check the "Known Conflict Patterns" section
3. Ask in the PR discussion with a specific reference to the concern

**When in doubt, ask before implementing.**
