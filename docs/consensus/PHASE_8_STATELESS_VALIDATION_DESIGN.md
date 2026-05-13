# Phase 8: Stateless Validation Design

**Status:** DESIGN DRAFT (Not Implemented)
**Branch:** `feature/utreexo-phase8-stateless-validation`
**Prerequisites:** Phase 7 (wire protocol) complete and sealed

---

## Goal

Enable nodes to fully validate blocks **without a UTXO database**, using only:
- Block data
- Utreexo proofs (in `BlockUtreexoData`)
- Accumulator roots (in block headers)

Invalid blocks MUST be rejected deterministically, regardless of validation mode.

---

## Validation Rules

### A. When Are Proofs Required?

**Post-Activation:**
- Proofs are REQUIRED in every block after `UTREEXO_ACTIVATION_HEIGHT`
- Pre-activation: Proofs are OPTIONAL (Phase 7 transport-only)

**Mode-Specific Behavior:**
- **Stateful nodes** (UTXO database): MAY ignore proofs, validate via database
- **Stateless nodes** (no database): MUST validate using proofs

**Rule:** A block missing `BlockUtreexoData` post-activation is INVALID (consensus rule).

---

### B. What Exactly Is Validated?

For each spent input in a block:

1. **UTXO Membership Proof**
   - Proof MUST demonstrate UTXO exists in previous block's accumulator
   - Proof MUST verify against `header.utreexoCommitment` (32-byte root)
   - Proof MUST match the exact outpoint being spent

2. **Script Validation**
   - Script execution happens AFTER proof verification
   - Same rules as stateful validation (no changes)

3. **Accumulator Root Continuity**
   - `Block[N].header.utreexoCommitment` = accumulator state AFTER applying Block[N]
   - Accumulator update MUST be deterministic (add outputs, delete inputs)
   - Root MUST match computed accumulator state

4. **Proof Structure Integrity**
   - Proof size ≤ `MAX_PROOF_SIZE` (10 MB)
   - Batch proofs MUST cover ALL non-coinbase inputs
   - No missing proofs, no extra proofs

---

### C. Failure Behavior (Consensus Rejections)

A block MUST be rejected if ANY of the following occur:

| Condition | Rejection Reason |
|-----------|------------------|
| `BlockUtreexoData` missing (post-activation) | `PROOF_MISSING` |
| Proof fails cryptographic verification | `PROOF_INVALID` |
| Proof root ≠ `header.utreexoCommitment` | `ROOT_MISMATCH` |
| Proof does not match spent outpoint | `PROOF_OUTPOINT_MISMATCH` |
| Accumulator update inconsistent | `ACCUMULATOR_STATE_ERROR` |
| Proof size > `MAX_PROOF_SIZE` | `PROOF_TOO_LARGE` |

**No soft failures. No fallback to UTXO database.**

Stateless nodes that fail validation CANNOT accept the block, even if stateful nodes accept it.
This is a **fork risk** if proofs are invalid but databases are correct.

---

## Activation Condition

```cpp
// Consensus parameter (chainparams.h)
consensus.UTREEXO_ACTIVATION_HEIGHT = TBD;  // Testnet: low value, Mainnet: high value

// Activation check
bool IsUtreexoActive(uint32_t block_height) {
    return block_height >= consensus.UTREEXO_ACTIVATION_HEIGHT;
}
```

**Pre-activation:**
- Proofs optional
- Accumulator updated in shadow mode
- No consensus enforcement

**Post-activation:**
- Proofs mandatory
- Stateless validation enforced
- Missing/invalid proofs = block rejection

---

## Stateful vs Stateless Behavior

### Stateful Node (UTXO Database)

**Validation Path:**
```
Block → Check UTXO DB → Validate Scripts → Accept/Reject
         ↓ (Optional: verify proofs for testing)
```

**Characteristics:**
- Uses existing `UTXOIndex` for input validation
- MAY verify proofs as a sanity check (not consensus-critical)
- Updates accumulator in parallel (for serving proofs to stateless nodes)
- Consensus-compatible with stateless nodes (both must reject same blocks)

---

### Stateless Node (No Database)

**Validation Path:**
```
Block → Extract BlockUtreexoData → Verify Proofs → Validate Scripts → Accept/Reject
         ↓ (MUST succeed or block rejected)
```

**Characteristics:**
- NO UTXO database
- MUST validate every proof
- Updates accumulator as only source of truth
- Relies on bridge nodes for proof data
- Disk usage: ~100 KB accumulator state vs ~10 GB UTXO database

**Proof Verification Algorithm:**
```cpp
for (auto& input : block.inputs) {
    // 1. Extract proof for this input
    UtreexoProof proof = block.utreexo_data.GetProofForInput(input.outpoint);

    // 2. Verify proof against previous accumulator root
    Hash256 prev_root = prev_block.header.utreexoCommitment;
    if (!utreexo_forest.Verify(proof, input.outpoint, prev_root)) {
        return REJECT_PROOF_INVALID;
    }

    // 3. Validate script (normal consensus)
    if (!VerifyScript(input, utxo_from_proof)) {
        return REJECT_SCRIPT_INVALID;
    }
}

// 4. Update accumulator (delete inputs, add outputs)
utreexo_forest.Modify(deleted_leaves, added_leaves);

// 5. Verify final root matches header
if (utreexo_forest.GetRoot() != block.header.utreexoCommitment) {
    return REJECT_ROOT_MISMATCH;
}
```

---

## Validation Mode Split

### Implementation

```cpp
// New enum (include/consensus/validation_mode.h)
enum class ValidationMode {
    STATEFUL,   // Uses UTXO database
    STATELESS   // Uses Utreexo proofs only
};

// Plumbed into:
// - BlockValidator (consensus/block_validation.h)
// - Chainstate manager (daemon/services/chainstate_service.h)
// - IBD logic (daemon/ibd_manager.h)
```

**Node Configuration:**
```bash
# Stateful node (default)
./dinerod --validation-mode=stateful

# Stateless node
./dinerod --validation-mode=stateless
```

**Invariant:** Both modes MUST accept/reject the same blocks post-activation.

---

## Consensus Invariants (MUST HOLD)

1. **Deterministic Rejection**
   - All nodes reject the same invalid blocks
   - Stateful and stateless nodes agree on validity

2. **Accumulator Continuity**
   - `Block[N].header.utreexoCommitment` deterministically computed from Block[N-1]
   - No accumulator forks

3. **Proof Completeness**
   - Every spent input has exactly one proof
   - No missing proofs, no duplicate proofs

4. **Script Equivalence**
   - Stateless validation executes identical script logic as stateful
   - Same signature checks, same locktime rules

5. **No UTXO Resurrection**
   - Once a UTXO is spent (proven deleted), it cannot be spent again
   - Accumulator prevents double-spends via membership proofs

---

## What Phase 8 Does NOT Include

Explicitly deferred to Phase 9+:

- ❌ Proof gossip optimization (flood vs on-demand)
- ❌ Proof compression (compact proofs, proof aggregation)
- ❌ Performance tuning (parallel verification, caching)
- ❌ Lightning integration (stateless channel validation)
- ❌ ZK proof validation (confidential transactions)
- ❌ Activation logic beyond simple height gate

Phase 8 goal: **Make stateless validation work correctly**, not efficiently.

---

## Testing Requirements

Phase 8 MUST pass all of these tests before merge:

### Consensus Tests (Ring 2 Extension)

| Test | Description | Expected Result |
|------|-------------|-----------------|
| `T8.1` | Block missing `BlockUtreexoData` post-activation | REJECT (`PROOF_MISSING`) |
| `T8.2` | Block with invalid proof (wrong sibling hashes) | REJECT (`PROOF_INVALID`) |
| `T8.3` | Proof root ≠ header commitment | REJECT (`ROOT_MISMATCH`) |
| `T8.4` | Proof for wrong outpoint (different txid) | REJECT (`PROOF_OUTPOINT_MISMATCH`) |
| `T8.5` | Valid proof, valid block | ACCEPT (stateless) |
| `T8.6` | Same block from T8.5 | ACCEPT (stateful) |
| `T8.7` | Accumulator update produces wrong root | REJECT (`ACCUMULATOR_STATE_ERROR`) |
| `T8.8` | Proof size exceeds `MAX_PROOF_SIZE` | REJECT (`PROOF_TOO_LARGE`) |

### Determinism Test

| Test | Description | Expected Result |
|------|-------------|-----------------|
| `T8.9` | Stateful and stateless nodes validate same 100-block chain | Identical accept/reject |

---

## Implementation Checklist

Phase 8 work items (in order):

- [ ] **8.1** Add `ValidationMode` enum and plumb into BlockValidator
- [ ] **8.2** Enforce `BlockUtreexoData` presence post-activation
- [ ] **8.3** Implement stateless proof verification path
- [ ] **8.4** Add consensus rejection codes (`PROOF_INVALID`, `ROOT_MISMATCH`, etc.)
- [ ] **8.5** Update accumulator state deterministically
- [ ] **8.6** Verify accumulator root continuity
- [ ] **8.7** Write tests T8.1 through T8.9
- [ ] **8.8** Document activation parameters in `chainparams.h`

**Status:** All items TODO (design phase only)

---

## Open Questions

1. **Activation Height**
   - Testnet: Block 1000? (for rapid testing)
   - Mainnet: Block 100,000? (6-12 months out)

2. **Bridge Node Discovery**
   - How do stateless nodes find bridge nodes reliably?
   - DNS seeds? Service bit filtering?

3. **Proof Availability**
   - What if no bridge node serves proofs for a block?
   - IBD stall vs reject block?

4. **Reorg Handling**
   - Accumulator state must rewind during reorgs
   - Phase 6 UtreexoDelta already handles this (verify correctness)

---

## Success Criteria

Phase 8 is complete when:

1. ✅ Stateless node validates 1000-block testnet chain using only proofs
2. ✅ Stateful and stateless nodes agree on validity for all test blocks
3. ✅ All 9 consensus tests (T8.1-T8.9) pass
4. ✅ No UTXO database required for block validation
5. ✅ Invalid proofs deterministically rejected

---

**Next Step:** Review this design, then implement Step 8.1 (ValidationMode split).
