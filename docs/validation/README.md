# Consensus Validation Archive

This directory contains test results and evidence for consensus layer validation.

---

## Validation Milestones

### December 13, 2025 - Deep Reorg Validation

**Status**: ✅ **PASSED**

**Test**: 100-block deep reorganization stress test
**File**: `deep_reorg_test_2025-12-13.log`

**What Was Proven**:
- ChainDB survived 100-block disconnect + 120-block reconnect
- UTXO set remained consistent across reorg
- TX index rollback worked correctly
- Longest chain rule enforced automatically
- Mining continued successfully after reorg

**Configuration**:
```
Fork point:    100 blocks
Chain A:       200 blocks (orphaned)
Chain B:       220 blocks (became active)
Reorg depth:   100 blocks
Final height:  221 blocks
```

**Invariants Proven**:
1. ChainDB is reorg-safe at depth 100+
2. Longest-chain selection is automatic
3. TX index is reorg-symmetric
4. UTXO set is reorg-consistent
5. BIP113 timestamp bounds enforced

---

## Test Files

### Reorg Tests
- `tests/test_tx_index_reorg.sh` - TX index rollback verification
- `tests/reorg/test_deep_reorg.sh` - 100-block deep reorg stress test

### Test Results
- `deep_reorg_test_2025-12-13.log` - Full execution log (PASSED)

---

## Consensus Freeze

**Date**: December 13, 2025

Consensus layer is **frozen** after successful validation.

**Reference**:
- `/CONSENSUS_VALIDATION_MILESTONE.md` (root)
- `/src/consensus/CONSENSUS_FROZEN.md`

**Policy**: Consensus may only be modified if a test proves a violation.

---

## Validation Criteria

To declare consensus validated, the following must pass:

✅ Genesis block generation and verification
✅ Block timestamp validation (BIP113)
✅ Chain selection (longest valid chain)
✅ TX index rollback on reorg
✅ UTXO set consistency across reorg
✅ ChainDB durability under deep reorg
✅ Mining continuation after reorg

**All criteria met**: December 13, 2025

---

## Next Phase

**Recommended**: Mempool policy hardening (non-consensus)
**Optional**: Crash-during-reorg recovery test
**Prohibited**: Consensus changes without proof

**Consensus layer is production-ready.**
