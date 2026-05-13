# Architecture Freeze Policy

**Status**: Normative
**Version**: 1.0
**Effective Date**: 2025-12-24

---

## Executive Summary

The **architectural layer is frozen**. Not the code — the rules.

Changes to normative architecture documents are **rare, explicit, and treated like consensus changes**.

This stability enables contributors to build on architectural guarantees without fear of retroactive invalidation.

---

## What Is Frozen

### Frozen (Rare Changes Only)

The following documents define **architectural law** and changes require the process defined in this policy:

- **`docs/architecture/layered_feature_compatibility.md`** (Normative)
  - Layer definitions (Layers 0-4)
  - The two mandatory invariants
  - Prohibited patterns list
  - Safe vs. dangerous pattern classifications

- **This document** (`docs/architecture/ARCHITECTURE_FREEZE_POLICY.md`)
  - Change process requirements
  - Approval criteria

### Not Frozen (Flexible)

The following are **reference documentation** and can be updated normally:

- Implementation guides
- Code examples
- Performance benchmarks
- Deployment documentation
- Non-normative explanatory text
- `docs/architecture/README.md` (index only, not rules)

---

## Why Freeze The Architecture

### Stability Enables Trust

**Contributors need to know the rules won't change underneath them.**

When architectural rules are stable:
- Developers can build features confident they won't be retroactively invalidated
- External contributors can learn the architecture once, not continuously
- Code reviews use consistent criteria over time
- Auditors can verify compliance against a fixed reference

### Preventing Erosion

**Architecture dies by a thousand small compromises.**

Without a freeze policy:
- "Just this once" exceptions accumulate
- Prohibited patterns become "technically allowed if we reword the doc"
- Reviewers face pressure to "update the rules to match reality"
- The normative document becomes descriptive instead of prescriptive

### The Right Kind of Rigidity

**This policy makes changing *rules* hard, not changing *code*.**

You can:
- ✅ Add new features freely (within the rules)
- ✅ Refactor implementations
- ✅ Optimize performance
- ✅ Fix bugs
- ✅ Improve documentation

You cannot:
- ❌ Change layer definitions casually
- ❌ Remove invariants without extraordinary justification
- ❌ Reclassify prohibited patterns as allowed
- ❌ Weaken enforcement requirements

---

## When Changes Are Allowed

### 1. Clarification (Low Bar)

**Allowed**: Making existing rules clearer without changing their meaning.

**Examples**:
- Adding explanatory examples to existing rules
- Fixing typos or grammatical errors
- Reorganizing sections for better readability
- Adding cross-references

**Process**: Standard PR review

**Test**: Would anyone who followed the old version violate the new version?
- If NO: This is clarification (allowed)
- If YES: This is a rule change (requires freeze process)

---

### 2. Extension (Medium Bar)

**Allowed**: Adding new rules that don't contradict existing ones.

**Examples**:
- Adding Layer 5 for a new category (e.g., "Layer 5: External Bridges")
- Documenting a new prohibited pattern discovered through research
- Adding specific guidance for a new feature class (e.g., "Sidechains")

**Process**: Architecture Review Process (see below)

**Test**: Do existing compliant implementations remain compliant?
- If YES: This is extension (allowed with review)
- If NO: This is a breaking change (requires extraordinary justification)

---

### 3. Breaking Change (High Bar)

**Allowed ONLY**: When the existing architecture is demonstrably wrong or blocks critical functionality.

**Examples** (hypothetical):
- Discovering that one of the two invariants is mathematically impossible to satisfy
- Finding that a "prohibited pattern" is actually required for security
- Proving that layer separation as defined prevents a necessary feature

**Process**: Architecture Review Process + Governance Vote (see below)

**Test**: Is this change fixing a mistake in the architecture itself?
- If YES: Breaking change may be justified
- If NO: Find a way to achieve the goal within existing rules

---

## Architecture Review Process

### For Extension and Breaking Changes

All changes to normative architecture documents (except clarifications) follow this process:

### Step 1: Proposal (GitHub Issue)

Create a GitHub issue with the label `architecture-change` containing:

1. **Motivation**: What problem does this solve?
2. **Current Rule**: Quote the existing normative text
3. **Proposed Change**: Specific new wording
4. **Classification**: Clarification / Extension / Breaking Change
5. **Impact Analysis**: What code/PRs would be affected?
6. **Alternatives Considered**: Why not solve this another way?

**Example Title**: `[Architecture Change] Add Layer 5 for External Bridge Protocols`

---

### Step 2: Public Discussion (Minimum 14 Days)

- Issue remains open for **at least 14 days** for community input
- Architecture discussion happens in the issue comments
- Goal: Understand if this is actually necessary or if the problem can be solved within existing rules

**Discussion Questions**:
- Is this truly a rule change or a clarification?
- Can we achieve the goal without changing the architecture?
- Does this contradict existing invariants?
- What's the long-term impact?

---

### Step 3: Architecture Committee Review

**Who Reviews**:
- Core maintainers
- Original architecture authors
- At least one external security researcher (if breaking change)

**Review Criteria**:
- [ ] Does this change preserve the two mandatory invariants?
- [ ] Is this the minimum necessary change?
- [ ] Are there alternatives within the existing architecture?
- [ ] Does this set a precedent we can live with long-term?
- [ ] Is the wording precise and enforceable?

**Decision**: Approve / Request Revisions / Reject

---

### Step 4: Implementation (If Approved)

1. **PR against normative docs** with the approved wording
2. **Changelog entry** documenting the architectural change
3. **Migration guide** if existing code is affected
4. **Announcement** in release notes marked as "Architecture Change"

**PR Requirements**:
- Must reference the approved GitHub issue
- Must include before/after comparison
- Must update version number in the normative document
- Must be reviewed by at least 2 architecture committee members

---

### Step 5: Governance Vote (Breaking Changes Only)

**For breaking changes**, after committee approval:

1. **Community Vote** (if governance structure exists)
2. **Announce** in all communication channels
3. **Grace Period** of at least 30 days before merge
4. **Deprecation Path** for old rule (if applicable)

**Approval Threshold**: 2/3 majority of active core contributors

---

## Special Cases

### Security-Critical Corrections

**If a normative rule is discovered to have a security flaw**, emergency changes are allowed:

1. **Immediate Fix**: PR to correct the security issue
2. **Retroactive Process**: Follow Architecture Review Process after the fact
3. **Documentation**: Explain why emergency change was necessary

**Example**: If invariant wording accidentally allows a consensus-critical vulnerability

---

### Experimental Features

**New experimental features** can be developed without architecture approval if:

1. Clearly marked as **experimental** (not production-ready)
2. Do not require changes to normative documents
3. Documented as "within Layer X but experimental"

**When experimental becomes production**: Must pass Architecture Review if it requires any rule changes

---

## Enforcement

### For Contributors

**Before submitting PR**:
- Check if your change requires architecture modification
- If yes: Open architecture change issue first, wait for approval
- If no: Proceed with implementation

### For Reviewers

**When reviewing PRs**:
- If PR requires architecture change but doesn't have approval: Request architecture review
- If PR violates frozen architecture: Reject with citation to normative document
- If PR attempts to "work around" architecture by rewording: Escalate to architecture committee

### For Maintainers

**Maintainers must**:
- Reject PRs that sneak architecture changes without following this process
- Protect the freeze policy from erosion
- Ensure community understands why architecture stability matters

---

## Rationale

### Why Not Just Use PRs?

**Normal PR review is insufficient for architecture changes** because:

- Architecture affects every future contributor
- Changes create precedent
- Mistakes are expensive to reverse
- Community needs time to understand implications

### Why Not Make Everything Frozen?

**We freeze rules, not implementations** because:

- Code needs to evolve and improve
- Performance optimizations should be easy
- Bug fixes can't wait 14 days
- Implementation details don't affect other contributors the same way rules do

### Why 14 Days Minimum?

**Two weeks allows**:
- International contributors across timezones to participate
- Thoughtful consideration, not reactive decisions
- Alternatives to be explored
- Implications to be understood

For breaking changes, 30 days ensures even occasional contributors can weigh in.

---

## Examples

### ✅ Allowed Without Process (Clarification)

**Change**: Add example to prohibited patterns
```markdown
- ❌ ZK proofs as consensus validation authorities
  Example: Using a ZK-SNARK to "prove" a transaction is valid
  without executing the script
```

**Why**: Makes existing rule clearer without changing its meaning

---

### ✅ Allowed With Review (Extension)

**Change**: Add Layer 5 for bridge protocols
```markdown
| Layer 5 | External bridges | Cross-chain protocols |
```

**Why**: Adds new category, doesn't contradict existing layers

**Process**: Architecture Review (14 day discussion, committee approval)

---

### ⚠️ Requires Governance Vote (Breaking Change)

**Change**: Remove invariant "Lower layers never trust higher layers"

**Why**: This would invalidate core architectural guarantees

**Process**: Architecture Review + 30 day grace + 2/3 governance vote

**Likely Outcome**: Rejected unless extraordinary justification

---

### ❌ Never Allowed

**Change**: Reword prohibited pattern to make it allowed because "we want to implement it"

**Why**: This is architecture erosion, not legitimate change

**Resolution**: Find a way to implement the feature within existing architecture, or make a proper case for why the prohibited pattern should be reclassified (requires breaking change process)

---

## Document History

| Date       | Version | Change                                    |
|------------|---------|-------------------------------------------|
| 2025-12-24 | 1.0     | Initial freeze policy established         |

---

## Meta-Rule

**This document is self-enforcing**: Changes to this freeze policy follow the same Architecture Review Process as changes to `layered_feature_compatibility.md`.

**Bootstrapping**: The initial version (1.0) is adopted by consensus of the architecture authors as of the commit that introduced it.

---

## Summary (TL;DR)

1. **Architecture rules are frozen** → Changes are rare and explicit
2. **Clarifications are easy** → Fix typos, add examples
3. **Extensions require review** → 14 day discussion + committee approval
4. **Breaking changes require governance** → 30 day grace + 2/3 vote
5. **Code is not frozen** → Build anything within the rules
6. **Stability enables trust** → Contributors can rely on architectural guarantees

**Goal**: Make architecture drift require conscious community decision, not happen by accident.
