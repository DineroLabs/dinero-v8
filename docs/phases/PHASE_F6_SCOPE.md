# Phase F.6 Scope Definition

**Phase**: F.6 - Wallet State & Persistence Certification
**Status**: Draft
**Purpose**: Define exact boundaries of what is and is not certified in this phase

---

## Executive Summary

Phase F.6 certifies **wallet database correctness** for confirmed chain state. This phase locks the Mining ↔ Wallet interaction boundary established in F.5 and ensures wallet state is deterministic, crash-safe, and reorg-safe.

**What this phase IS**: Correctness certification for wallet persistence
**What this phase IS NOT**: Feature development, UI improvements, or performance tuning

---

## IN SCOPE (Must Be Certified)

### 1. Wallet Database Correctness

**What**: Wallet DB accurately reflects blockchain state
**Why**: Core trust boundary - user funds depend on this
**Validates**: Invariants W.1, W.2

**Specific behaviors**:
- Balance equals sum of confirmed UTXOs
- UTXO set matches blockchain state
- Transaction history is complete and accurate
- Wallet state persists across restarts

### 2. Restart Safety

**What**: Daemon restart does not corrupt or change wallet state
**Why**: Users must be able to safely stop/start daemon
**Validates**: Invariant W.2

**Specific behaviors**:
- No UTXO loss on restart
- No UTXO duplication on restart
- No phantom balances after restart
- Transaction history unchanged if chain unchanged

### 3. Crash Consistency

**What**: Unclean shutdown does not corrupt wallet
**Why**: System crashes are inevitable, wallet must survive
**Validates**: Invariant W.3

**Specific behaviors**:
- Wallet DB opens after crash
- State is either "before write" or "after write", never partial
- No torn writes visible to user

**Note**: This may be validated through code review + RocksDB atomicity guarantees rather than explicit crash injection tests.

### 4. Chain Reorganization Handling

**What**: Wallet correctly handles blockchain reorgs
**Why**: Consensus layer event that wallet must respect
**Validates**: Invariant W.4

**Specific behaviors**:
- Orphaned UTXOs removed from wallet
- New canonical UTXOs added to wallet
- Balance after reorg matches rescan result
- Reorg depths 1-3 blocks handled correctly

### 5. Mining Reward Attribution

**What**: Coinbase outputs to wallet addresses are tracked correctly
**Why**: Locks Mining ↔ Wallet boundary from F.5
**Validates**: Invariant W.5

**Specific behaviors**:
- Coinbase UTXO appears in wallet
- Coinbase matures after 100 blocks
- Orphaned coinbase disappears on reorg
- Mining rewards persist across restarts

**Dependencies**: Requires F.5 (mining subsystem) to be stable

### 6. Rescan Idempotency

**What**: Wallet rescan is safe to run multiple times
**Why**: Rescan is a recovery operation, must be repeatable
**Validates**: Invariant W.6

**Specific behaviors**:
- Multiple rescans produce identical state
- No transaction duplication
- No balance changes on repeated rescan

### 7. Scope Limitation Enforcement

**What**: Only confirmed chain state is persisted
**Why**: Prevents ambiguity about wallet guarantees
**Validates**: Invariant W.7

**Specific behaviors**:
- Mempool transactions NOT persisted to wallet DB
- Unconfirmed state does NOT survive restart
- Only canonical chain affects wallet state

---

## OUT OF SCOPE (Explicitly Excluded)

### 1. Lightning Network State (Non-Negotiable)

**Why**: Lightning is Layer 3, requires separate certification
**Deferred to**: Future Lightning certification phase
**Reason**: Different persistence model, different trust assumptions

### 2. Mempool Transaction Tracking (Non-Negotiable)

**Why**: Mempool state is inherently non-deterministic
**Covered by**: W.7 scope limitation
**Note**: Wallet may show pending transactions in UI, but NOT persist them

### 3. Wallet Encryption (Non-Negotiable)

**Why**: Separate security concern, not persistence correctness
**Deferred to**: Future wallet security phase
**Reason**: Encryption is orthogonal to state correctness

### 4. GUI Behavior (Non-Negotiable)

**Why**: UI is presentation layer, not persistence layer
**Out of scope**: Transaction sorting, display order, formatting
**Note**: GUI may have bugs, but wallet DB must be correct

### 5. Performance Optimization (Non-Negotiable)

**Why**: F.6 certifies correctness, not speed
**Deferred to**: Future performance phase
**Examples excluded**:
- Rescan speed
- Database query optimization
- Index tuning
- Cache strategies

### 6. Wallet Features (Non-Negotiable)

**Why**: Feature development is separate from persistence certification
**Examples excluded**:
- Multi-signature wallets
- HD wallet derivation paths
- Address labeling
- Coin control
- Privacy features (CoinJoin, etc.)

**Note**: These may be added later, but F.6 only certifies basic wallet persistence

### 7. Unconfirmed Transaction Handling (Non-Negotiable)

**Why**: Covered by W.7 - explicitly out of scope
**Not certified**:
- Mempool transaction persistence
- Replace-by-fee handling
- Transaction conflict resolution
- Unconfirmed balance calculations

### 8. External Wallet Integrations (Non-Negotiable)

**Why**: External systems have their own persistence guarantees
**Examples excluded**:
- Hardware wallet integration
- Watch-only wallet sync
- BIP39 seed import/export
- External backup formats

---

## Certification Criteria

Phase F.6 is considered **certified** only when:

1. ✅ All 10 P0 tests pass (per test matrix)
2. ✅ Daemon starts and runs without crashes
3. ✅ No known wallet DB corruption scenarios
4. ✅ Mining ↔ Wallet interaction verified (builds on F.5)
5. ✅ Documentation complete (invariants + tests + certification)
6. ✅ Code review confirms crash consistency (or explicit test passes)

**Minimum bar**: 10/10 P0 tests passing + no known correctness issues in scope

---

## What Success Looks Like

After F.6 certification:

### Users can trust that:
- Wallet balance is always correct for confirmed transactions
- Restarting daemon is safe
- Blockchain reorgs are handled correctly
- Mining rewards appear and mature correctly

### Users cannot assume:
- Mempool transactions will survive restart
- Wallet is encrypted
- Performance is optimal
- All wallet features are implemented

### Developers can rely on:
- Wallet DB invariants (W.1–W.7) are locked
- Mining ↔ Wallet boundary is stable
- Tests will catch persistence regressions
- Future wallet features build on solid foundation

---

## Scope Discipline Mechanisms

To prevent scope creep:

1. **All work must map to an invariant (W.1–W.7)**
   - If it doesn't map to an invariant, it's out of scope

2. **All tests must map to the test matrix (T1–T11)**
   - No "while we're here" tests

3. **No feature development during certification**
   - Certification locks existing behavior
   - New features come after certification

4. **No performance work during certification**
   - "Make it right" before "make it fast"

5. **Document review must cite this scope doc**
   - Any PR touching wallet must reference IN SCOPE items
   - Reviewers can reject out-of-scope work by citation

---

## Relationship to Other Phases

### Depends on:
- **F.5 (Mining subsystem)**: Required for W.5 (mining rewards)

### Enables:
- **Future Lightning phases**: Wallet persistence is prerequisite
- **Future wallet features**: HD wallets, multisig, etc. build on this
- **Future privacy features**: CT, CoinJoin require correct base wallet

### Independent of:
- **Consensus rules**: Wallet persistence is orthogonal to consensus
- **Network protocol**: P2P layer doesn't affect wallet DB
- **RPC API**: API may change, wallet persistence invariants don't

---

## Timeline Expectations

**This phase does NOT have a timeline.**

Per RELEASE_POLICY.md, phases are scope-based, not time-based.

F.6 is complete when:
- All in-scope items are certified
- All out-of-scope items are documented
- All tests pass
- Certification is archived

This could take 1 week or 3 months. The timeline is irrelevant.

---

## Approval Process

This scope document must be approved before implementation begins.

**Approval means**:
- In-scope items are agreed upon
- Out-of-scope items are non-negotiable
- No scope changes without explicit re-approval

**Scope changes require**:
- Clear justification (bug found, dependency discovered)
- Updated scope document
- Re-approval before proceeding

---

**Document Version**: 1.0
**Last Updated**: 2025-12-29
**Maintained By**: DineroCoin Engineering Team
