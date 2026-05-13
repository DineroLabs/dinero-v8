# DineroCoin Mainnet Readiness Checklist

**Version**: 1.0
**Last Updated**: 2026-01-22
**Status**: PRE-LAUNCH

---

## Golden Rule (Non-Negotiable)

> **Nothing ships without surviving the regtest protocol.**
> No mocks. No stubs. No shortcuts.
> Unit tests prove logic. Regtest proves reality.

---

## Quick Start

```bash
# Run the full regtest gate test
./scripts/regtest_mainnet_gate.sh

# With verbose output
./scripts/regtest_mainnet_gate.sh --verbose

# Keep datadir for debugging
./scripts/regtest_mainnet_gate.sh --skip-cleanup
```

---

## Phase Checklist

### Phase 1: Chain & Consensus Reality Checks

| Test | Description | Status |
|------|-------------|--------|
| 1.1 | Genesis block exists at height 0 | ⬜ |
| 1.2 | Chain reports as "regtest" | ⬜ |
| 1.3 | Genesis has no previous block | ⬜ |
| 1.4 | Genesis merkle root is deterministic | ⬜ |
| 1.5 | Mining produces valid blocks | ⬜ |
| 1.6 | Coinbase subsidy matches schedule | ⬜ |
| 1.7 | Coinbase maturity enforced (100 blocks) | ⬜ |

**Consensus-Critical Invariants**:
- [ ] Utreexo active from height 2 (mainnet/testnet)
- [ ] Utreexo active from height 0 (regtest)
- [ ] Witness commitment enforced from height 2
- [ ] 128-byte headers enforced
- [ ] ASERT difficulty adjustment working

---

### Phase 2: Wallet Reality (User Safety)

| Test | Description | Status |
|------|-------------|--------|
| 2.1 | Wallet creation succeeds | ⬜ |
| 2.2 | Wallet loads without crash | ⬜ |
| 2.3 | Address generation works | ⬜ |
| 2.4 | Addresses are unique | ⬜ |
| 2.5 | Address validation correct | ⬜ |
| 2.6 | HD derivation path correct | ⬜ |
| 2.7 | Wallet backup/restore works | ⬜ |
| 2.8 | Rescan recovers exact balance | ⬜ |

**User-Critical Invariants**:
- [ ] No duplicate UTXOs after rescan
- [ ] Balance matches actual UTXOs
- [ ] Taproot (P2TR) addresses work
- [ ] Hardware wallet signing works (Ledger/Trezor)

---

### Phase 3: Transactions & Mempool

| Test | Description | Status |
|------|-------------|--------|
| 3.1 | Transaction broadcast succeeds | ⬜ |
| 3.2 | Transaction appears in mempool | ⬜ |
| 3.3 | Transaction confirms in block | ⬜ |
| 3.4 | Fee policy enforced | ⬜ |
| 3.5 | RBF works (if enabled) | ⬜ |
| 3.6 | CPFP deterministic | ⬜ |

**Consensus-Critical Invariants**:
- [ ] No transaction mutation
- [ ] Same mempool → same template (determinism)
- [ ] Witness data preserved through pipeline

---

### Phase 4: Reorg & Chain Safety

| Test | Description | Status |
|------|-------------|--------|
| 4.1 | Kill/restart preserves chain | ⬜ |
| 4.2 | Kill/restart preserves wallet | ⬜ |
| 4.3 | No DB corruption after crash | ⬜ |
| 4.4 | Mempool resurrects correctly | ⬜ |
| 4.5 | Reorg updates wallet balance | ⬜ |

**Safety-Critical Invariants**:
- [ ] Utreexo delta undo works (Phase 4)
- [ ] Block undo data correct
- [ ] No balance drift after chaos

---

### Phase 5: RPC & CLI Safety

| Test | Description | Status |
|------|-------------|--------|
| 5.1 | getblockchaininfo works | ⬜ |
| 5.2 | getnetworkinfo works | ⬜ |
| 5.3 | getmempoolinfo works | ⬜ |
| 5.4 | Invalid method rejected | ⬜ |
| 5.5 | Invalid params rejected | ⬜ |
| 5.6 | No crash on malformed input | ⬜ |

**Operator-Critical Invariants**:
- [ ] Meaningful error messages
- [ ] No undefined behavior
- [ ] Network mismatch detection

---

### Phase 6: Lightning Readiness (L1 → L2)

| Test | Description | Status |
|------|-------------|--------|
| 6.1 | Context RPC available | ⬜ |
| 6.2 | Network verification works | ⬜ |
| 6.3 | Channel-like UTXOs spendable | ⬜ |
| 6.4 | Correct sighash for LN | ⬜ |

**L2-Critical Invariants**:
- [ ] No L1 wallet dependency leaks
- [ ] Script forms compatible with LN

---

### Phase 7: GUI Reality (User-Facing)

| Test | Description | Status |
|------|-------------|--------|
| 7.1 | dinero-qt launches | ⬜ BLOCKED (no Qt build) |
| 7.2 | Daemon start/stop works | ⬜ |
| 7.3 | Wallet loads in GUI | ⬜ |
| 7.4 | Send/Receive works | ⬜ |
| 7.5 | Transactions update live | ⬜ |
| 7.6 | Reorg reflected in UI | ⬜ |
| 7.7 | No frozen UI on rescan | ⬜ |

---

### Phase 8: Chaos Test (No Mercy)

| Test | Description | Status |
|------|-------------|--------|
| 8.1 | 100 mine/send/kill cycles | ⬜ |
| 8.2 | No crashes | ⬜ |
| 8.3 | No balance drift | ⬜ |
| 8.4 | No consensus divergence | ⬜ |

---

## Mainnet Launch Gate (Hard Requirements)

### MUST PASS (Non-Negotiable)

- [ ] All regtest phases pass (except Phase 7 if no GUI)
- [ ] Two independent machines reproduce results
- [ ] No manual intervention required
- [ ] All failures are explainable and fixed

### SHOULD PASS (Strongly Recommended)

- [ ] Testnet running for 1+ week with no issues
- [ ] At least 2 independent node implementations sync
- [ ] Mining pool tested on testnet
- [ ] Exchange integration tested

### Consensus Rules Frozen

- [ ] Genesis block hash documented
- [ ] Activation heights documented (height 2)
- [ ] Witness commitment format documented
- [ ] Utreexo format documented
- [ ] Subsidy schedule documented

---

## How to Use This Checklist

1. **Before Launch**: Run `./scripts/regtest_mainnet_gate.sh`
2. **Mark Status**: ✅ = Pass, ❌ = Fail, ⬜ = Not tested
3. **Document Failures**: Add notes for any failures
4. **Repeat**: Fix issues and re-run until all pass
5. **Sign Off**: Two people must verify independently

---

## Sign-Off

| Role | Name | Date | Status |
|------|------|------|--------|
| Lead Developer | | | ⬜ |
| Security Reviewer | | | ⬜ |
| Independent Tester | | | ⬜ |

---

## Notes

_Add any test failures, edge cases, or observations here._

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-01-22 | Initial checklist |
