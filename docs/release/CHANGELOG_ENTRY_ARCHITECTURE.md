# Concise CHANGELOG Entry - Architecture Enforcement

**Drop this directly into CHANGELOG.md for the next release**

---

## [Unreleased] - Architecture Governance

### 🏛️ Canonical Architecture Documentation (Normative)

**Status**: ✅ Complete — Layered architecture formalized with enforcement mechanisms

This release establishes normative architectural documentation preventing consensus drift through explicit, enforceable invariants.

### Added

- **Layered Feature Compatibility Specification** (`docs/architecture/layered_feature_compatibility.md`)
  - Normative document defining 5-layer model (Consensus → State → Privacy → Off-chain → UX)
  - Two mandatory invariants: (1) Lower layers never trust higher, (2) Higher layers never weaken lower
  - Prevents ZK-based consensus shortcuts, snapshot-implied validity, wallet-driven consensus assumptions
  - Ensures safe interaction: Taproot + Covenants + Utreexo + ZK + Lightning

- **Architecture Documentation Index** (`docs/architecture/README.md`)
  - Central hub for all architectural documentation
  - Marks normative vs. reference documents
  - Compliance requirements for contributors/reviewers

- **PR Architecture Review Checklist** (`.github/ARCHITECTURE_REVIEW_CHECKLIST.md`)
  - Layer-specific compliance verification
  - Prohibited pattern detection guide
  - Objective approval/rejection criteria via citation

- **README Architecture Section** (`README.md`)
  - High-visibility layer model and invariants
  - Direct links to normative documentation

- **Contributing Guidelines Update** (`docs/CONTRIBUTING.md`)
  - Architecture Invariants section with compliance checklist
  - Explicit rejection policy for boundary violations

### Changed

- **Architecture is now enforceable, not just documented**
  - Before: Architectural rules were implicit knowledge
  - After: Reviewers can reject PRs by citation: *"Violates layered_feature_compatibility.md §3, invariant 1"*
  - PRs touching consensus/state/privacy/off-chain require architecture review against checklist

### Technical Details

**The Two Invariants** (prevent all dangerous cross-layer interactions):
1. **Lower layers never trust higher layers** → Prevents ZK/snapshot/wallet shortcuts in consensus
2. **Higher layers never weaken lower layers** → Prevents off-chain/privacy from bypassing validation

**Enforcement Chain**:
```
README → Architecture Index → Normative Spec → Contributing Guide → PR Checklist
```

**Why This Matters**:
- Changes failure mode from "someone might make a mistake" to "process prevents the mistake"
- Enables safe parallel development across layers
- External contributors inherit constraints automatically
- Audits verify compliance mechanically via normative reference

### Commits

- `42473615` - Add canonical architecture documentation and enforcement system

---

**Integration Note**: This is enforcement infrastructure, not just documentation. Signals project maturity and enables safe addition of advanced features without architectural risk.
