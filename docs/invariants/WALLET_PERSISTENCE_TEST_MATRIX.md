# Wallet Persistence Test Matrix

**Status**: Draft
**Purpose**: Define minimal test coverage for wallet persistence invariants (W.1–W.7)
**Principle**: Every invariant must have at least one explicit test. No implicit coverage.

---

## Test Coverage Map

| Invariant | Test ID | Test Name | Type | Priority |
|-----------|---------|-----------|------|----------|
| W.1 | T1 | Balance determinism after restart | Integration | P0 |
| W.1 | T2 | Balance determinism after rescan | Integration | P0 |
| W.2 | T3 | Restart with unchanged chain | Integration | P0 |
| W.3 | T4 | Crash during wallet write | Integration | P0 |
| W.4 | T5 | Chain reorg (depth 1) | Integration | P0 |
| W.4 | T6 | Chain reorg (depth 3) | Integration | P1 |
| W.5 | T7 | Mining reward appears in wallet | E2E | P0 |
| W.5 | T8 | Mining reward matures correctly | E2E | P0 |
| W.5 | T9 | Orphaned mining reward disappears | E2E | P0 |
| W.6 | T10 | Rescan twice (idempotency) | Integration | P0 |
| W.7 | T11 | Mempool tx not persisted | Unit | P0 |

**Priority Levels**:
- **P0**: Must pass for certification (blocking)
- **P1**: Should pass for certification (non-blocking if documented)

---

## Detailed Test Specifications

### T1: Balance Determinism After Restart (W.1)

**Validates**: Balance is deterministic function of chain + keys + DB

**Setup**:
1. Start daemon with fresh wallet
2. Mine 10 blocks to wallet address
3. Record balance B1
4. Stop daemon cleanly

**Test**:
1. Restart daemon (no chain changes)
2. Query balance B2

**Success Criteria**:
- B1 == B2
- No UTXOs lost
- No UTXOs duplicated

**Failure Modes**:
- Balance changed after restart
- Missing UTXOs
- Phantom UTXOs

---

### T2: Balance Determinism After Rescan (W.1)

**Validates**: Balance is deterministic function of chain state

**Setup**:
1. Wallet with known balance B1
2. Known chain state at height H

**Test**:
1. Perform wallet rescan from genesis
2. Record balance B2 at height H

**Success Criteria**:
- B1 == B2
- Transaction history identical

**Failure Modes**:
- Balance differs after rescan
- Missing transactions
- Duplicate transactions

---

### T3: Restart With Unchanged Chain (W.2)

**Validates**: Restart does not change wallet state if chain unchanged

**Setup**:
1. Wallet with 5 UTXOs
2. Known transaction history
3. Known balance

**Test**:
1. Record complete wallet state (UTXOs, history, balance)
2. Stop daemon cleanly
3. Restart daemon (no new blocks)
4. Record wallet state again

**Success Criteria**:
- UTXO set identical
- Transaction history identical
- Balance identical

**Failure Modes**:
- UTXO count changed
- Transactions missing or duplicated
- Balance changed

---

### T4: Crash During Wallet Write (W.3)

**Validates**: Crash does not corrupt wallet state (crash consistency)

**Setup**:
1. Wallet in known good state S1
2. Initiate wallet-modifying operation (receive tx, rescan, etc.)
3. SIGKILL daemon mid-operation

**Test**:
1. Restart daemon
2. Check wallet state S2

**Success Criteria**:
- S2 is either S1 (before write) or completed state (after write)
- S2 is never corrupted or contradictory
- Wallet DB opens successfully

**Failure Modes**:
- Wallet DB corruption
- Partial writes visible
- Daemon fails to start

**Note**: This requires test harness to inject SIGKILL at specific points

---

### T5: Chain Reorg (Depth 1) (W.4)

**Validates**: Wallet correctly handles single-block reorg

**Setup**:
1. Mine block B1 with tx sending 50 coins to wallet
2. Wallet sees B1, balance = 50
3. Trigger reorg: mine competing block B1' (no tx to wallet)
4. Mine B2' on top of B1' (longer chain)

**Test**:
1. Check wallet balance after reorg

**Success Criteria**:
- Balance = 0 (B1's tx is orphaned)
- Orphaned UTXO removed from wallet
- No phantom balance

**Failure Modes**:
- Wallet still shows 50 (stale UTXO)
- Wallet crashes
- Balance incorrect

---

### T6: Chain Reorg (Depth 3) (W.4)

**Validates**: Wallet correctly handles deep reorg

**Setup**:
1. Mine blocks B1, B2, B3 with txs to wallet in each
2. Wallet balance = 150
3. Trigger reorg: mine competing chain B1', B2', B3', B4' (no txs to wallet)

**Test**:
1. Check wallet balance after reorg

**Success Criteria**:
- Balance = 0 (all orphaned)
- All 3 orphaned UTXOs removed
- Wallet rescan produces same result

**Failure Modes**:
- Partial UTXO removal
- Wallet inconsistency
- Crash

**Priority**: P1 (depth-3 reorgs are rare, but should work)

---

### T7: Mining Reward Appears In Wallet (W.5)

**Validates**: Coinbase outputs to wallet-owned address are tracked

**Setup**:
1. Start mining to wallet address A
2. Mine 1 block

**Test**:
1. Query wallet for coinbase UTXO

**Success Criteria**:
- Coinbase UTXO present
- Amount = 50 (or configured block reward)
- UTXO marked as immature (not spendable yet)

**Failure Modes**:
- Coinbase UTXO missing
- Amount incorrect
- UTXO marked as mature (should be immature)

---

### T8: Mining Reward Matures Correctly (W.5)

**Validates**: Coinbase outputs mature after 100 blocks

**Setup**:
1. Mine 1 block with coinbase to wallet (height H)
2. Mine 99 more blocks (height H+99)

**Test**:
1. Check coinbase UTXO maturity at H+99 (should be immature)
2. Mine 1 more block (height H+100)
3. Check coinbase UTXO maturity at H+100 (should be mature)

**Success Criteria**:
- UTXO immature at H+99
- UTXO mature at H+100
- Balance reflects maturity

**Failure Modes**:
- UTXO never matures
- UTXO matures early
- Balance calculation ignores maturity

---

### T9: Orphaned Mining Reward Disappears (W.5)

**Validates**: Orphaned coinbase outputs are removed from wallet

**Setup**:
1. Mine block B with coinbase to wallet
2. Wallet shows coinbase UTXO
3. Trigger reorg: orphan block B

**Test**:
1. Check wallet after reorg

**Success Criteria**:
- Coinbase UTXO removed
- Balance excludes orphaned coinbase
- Rescan produces same result

**Failure Modes**:
- Coinbase UTXO persists (phantom balance)
- Wallet crashes
- Balance incorrect

---

### T10: Rescan Twice (Idempotency) (W.6)

**Validates**: Rescan is idempotent (safe to run multiple times)

**Setup**:
1. Wallet with known state (balance, UTXOs, history)
2. Known chain at height H

**Test**:
1. Perform rescan from genesis, record state S1
2. Perform rescan from genesis again, record state S2

**Success Criteria**:
- S1 == S2 (identical)
- No duplicate transactions
- No duplicate UTXOs
- Balance unchanged

**Failure Modes**:
- Duplicate transactions after second rescan
- Balance doubles
- UTXO count increases

---

### T11: Mempool Tx Not Persisted (W.7)

**Validates**: Unconfirmed transactions are not persisted to wallet DB

**Setup**:
1. Wallet with balance 100
2. Receive unconfirmed tx (+50, in mempool only)
3. Wallet may show pending balance 150

**Test**:
1. Stop daemon cleanly (tx still in mempool)
2. Restart daemon (clear mempool)
3. Check wallet balance

**Success Criteria**:
- Balance = 100 (confirmed only)
- Unconfirmed tx not in wallet DB
- No phantom balance

**Failure Modes**:
- Balance = 150 after restart (mempool state persisted)
- Unconfirmed tx appears as confirmed

**Note**: This test validates scope limitation (W.7)

---

## Test Implementation Strategy

### Phase 1: Unit Tests
- T11 (mempool exclusion)

### Phase 2: Integration Tests
- T1, T2, T3 (restart safety, determinism)
- T5, T6 (reorg handling)
- T10 (rescan idempotency)

### Phase 3: End-to-End Tests
- T7, T8, T9 (mining reward attribution, builds on F.5)

### Phase 4: Chaos Tests
- T4 (crash consistency, requires special harness)

**Recommendation**: Implement Phases 1-3 for initial certification. Phase 4 (T4) can be deferred if crash consistency is validated through code review and RocksDB atomicity guarantees.

---

## Certification Criteria

For Phase F.6 certification, the following tests must pass:

**Mandatory (P0)**:
- T1, T2, T3, T5, T7, T8, T9, T10, T11

**Optional (P1)**:
- T6 (deep reorg)

**Deferred**:
- T4 (crash consistency) - may be validated through code review instead

**Success Threshold**: 10/10 P0 tests passing

---

## Out of Scope

The following are explicitly **not** tested (per W.7):
- Mempool state persistence
- Unconfirmed transaction tracking across restarts
- Lightning channel state
- UI presentation order
- Performance benchmarks

---

**Document Version**: 1.0
**Last Updated**: 2025-12-29
**Maintained By**: DineroCoin Engineering Team
