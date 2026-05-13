# Wallet State & Persistence Invariants

**Status**: Draft
**Scope**: Wallet database correctness, restart safety, reorg handling, mining reward attribution
**Purpose**: Define what must never be violated in wallet state management

---

## Purpose

These invariants establish the behavioral guarantees for DineroCoin wallet state persistence. Each invariant represents a property that must hold across daemon restarts, chain reorganizations, and mining operations.

Violations of these invariants constitute critical bugs requiring immediate remediation.

---

## Invariants

### W.1 — Deterministic Balance Invariant

**Statement**: At any time, the wallet balance is a deterministic function of:
- the blockchain state
- the wallet's persisted keys
- the persisted wallet database

**Violations forbidden**:
- Balance differs after restart
- Balance differs after rescan with identical chain
- Balance differs across nodes with same data

**Rationale**: Wallet state must be reproducible from its inputs. Non-determinism indicates data corruption or logic errors.

---

### W.2 — Restart Safety Invariant

**Statement**: Restarting the daemon must not change wallet state unless the blockchain state has changed.

**Specifically**:
- No loss of UTXOs
- No duplication of UTXOs
- No phantom balances
- No missing history entries

**Rationale**: Restart is a lifecycle operation, not a state-changing event. If blockchain state is identical before and after restart, wallet state must also be identical.

---

### W.3 — Crash Consistency Invariant

**Statement**: An unclean shutdown (SIGTERM / crash) must not corrupt wallet state.

**Guarantees**: Wallet DB either reflects:
- state before last write, or
- state after last write

Never a partial or contradictory state.

**Rationale**: This is the "no torn writes" invariant. Wallet must be crash-safe through atomic persistence operations.

---

### W.4 — Chain Reorg Safety Invariant

**Statement**: Wallet state must correctly reflect chain reorganizations.

**Required behaviors**:
- Orphaned UTXOs are removed
- New canonical UTXOs are added
- Balance after reorg equals balance from clean rescan

**Rationale**: Wallet must track the canonical chain. Reorgs are consensus-layer events that wallet must respect.

---

### W.5 — Mining Reward Attribution Invariant

**Statement**: Coinbase outputs mined to wallet-owned addresses must:
- appear exactly once
- mature correctly
- persist across restarts
- disappear correctly if orphaned

**Rationale**: This locks Mining ↔ Wallet interaction, building directly on F.5. Mining rewards are a critical wallet input and must be handled atomically.

---

### W.6 — Idempotent Rescan Invariant

**Statement**: Re-running wallet rescan against the same chain must be idempotent.

**Forbidden**:
- Duplicate transactions
- Changing balances
- Reordering history in a way that affects totals

**Rationale**: Rescan is a recovery operation. It must be safe to run multiple times without side effects.

---

### W.7 — Scope Limitation Invariant

**Statement**: Wallet persistence guarantees apply only to:
- confirmed chain state
- canonical chain
- persisted wallet DB

**Explicitly out of scope**:
- Mempool-only state
- Unconfirmed transactions
- Lightning state
- UI presentation order

**Rationale**: This prevents future ambiguity about what the wallet subsystem guarantees. Unconfirmed state is inherently non-deterministic.

---

## Verification Strategy

Each invariant will be verified through:
1. **Unit tests**: Isolated component behavior
2. **Integration tests**: Cross-subsystem interactions (e.g., Mining → Wallet)
3. **End-to-end tests**: Full daemon lifecycle scenarios
4. **Certification**: Documented validation of all invariants before release

---

## Relationship to Other Invariants

- **Builds on Phase F.5**: Mining subsystem must be stable for W.5 to be testable
- **Prerequisite for Lightning**: Lightning state management requires wallet persistence correctness
- **Consensus-independent**: These invariants hold regardless of consensus rule changes

---

## Maintenance

This document is **normative** and changes require the same process as `RELEASE_POLICY.md`:
- Clarifications: Standard PR review
- Extensions: 14-day review period
- Breaking changes: 30-day review + community approval

---

**Document Version**: 1.0
**Last Updated**: 2025-12-29
**Maintained By**: DineroCoin Engineering Team
