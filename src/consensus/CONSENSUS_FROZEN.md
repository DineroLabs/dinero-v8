# ⚠️ CONSENSUS LAYER FROZEN

**Date**: December 13, 2025
**Milestone**: Deep Reorg Validation Complete
**Authority**: CONSENSUS_VALIDATION_MILESTONE.md (root)

---

## WARNING

Files in this directory are **frozen** and may only be modified if a test proves a consensus violation.

**Before modifying any file in `src/consensus/`, you must**:

1. ✅ Identify the specific consensus violation (with test case)
2. ✅ Prove your fix resolves the violation (with passing test)
3. ✅ Re-run full validation suite (`tests/reorg/test_deep_reorg.sh`)
4. ✅ Update `CONSENSUS_VALIDATION_MILESTONE.md` with the change

**If you cannot satisfy all four criteria, DO NOT modify consensus code.**

---

## What Is Frozen (Proven Correct)

### Block Validation
- `block_validation.cpp` - Block acceptance rules
- `timestamp_validation.hpp` - BIP113 timestamp bounds

### Chain Management
- `chain_manager.cpp` - Chain selection, reorg logic
- `block_index.cpp` - Chain state tracking

### UTXO Management
- `undo.cpp` - Reorg rollback symmetry
- Block connect/disconnect logic

### Genesis
- `chainparams_impl.cpp` - Network genesis parameters

### Write Authorization
- `ChainWriteToken` - Compile-time write authority

---

## What Was Proven

✅ 100-block deep reorg survived
✅ ChainDB remained consistent
✅ UTXO set matched chain tip
✅ TX index rollback worked
✅ Longest chain rule enforced
✅ Timestamp bounds enforced (BIP113)
✅ Genesis is canonical

**Test**: `tests/reorg/test_deep_reorg.sh`
**Result**: PASSED (December 13, 2025)

---

## If You Need To Change Consensus

**Ask yourself**:
1. Is this fixing a proven bug, or improving ergonomics?
2. Can this be done in the policy layer instead?
3. Have I written a failing test first?
4. Am I certain this won't break the deep reorg test?

**If unsure**: Ask. Don't modify.

---

## Exemptions

The following are **NOT frozen** (non-consensus):

- **Logging**: Add debug output freely
- **Comments**: Improve documentation
- **Variable names**: Refactor for clarity (no logic changes)
- **Performance**: Optimize without changing behavior
- **Tests**: Add more validation

**Rule**: If it doesn't change what blocks are accepted or how reorgs work, it's not frozen.

---

## Rationale

From `CONSENSUS_VALIDATION_MILESTONE.md`:

> "Most projects never get here. They stop at 'it syncs', 'it mines', 'it didn't crash this time'. You went further: You forced the chain to lie to itself, you forced it to undo history, you forced it to choose again. And it chose correctly. That's the difference between software and a protocol implementation."

**This freeze protects that achievement.**

---

## Contact

If you believe you've found a consensus violation:
1. Write a test that proves it
2. Document the violation clearly
3. Propose a minimal fix
4. Re-run validation suite

**Burden of proof is on the proposer.**
