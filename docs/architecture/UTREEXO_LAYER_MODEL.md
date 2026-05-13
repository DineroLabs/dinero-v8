# How Utreexo Fits Into The Layered Architecture

**Date**: 2025-12-24
**Purpose**: Explain Utreexo's role in the layer model and dependencies on Layer 0

---

## TL;DR

**Utreexo is Layer 1 (State Representation)** - It changes **HOW** UTXO state is stored/proven, not **WHAT** is valid.

**Critical Dependency**: Utreexo commitments are calculated **AFTER** Layer 0 validates transactions. If Layer 0 accepts invalid covenant transactions, Utreexo would commit to invalid state.

**Why Layer 0 fixes mattered**: Invalid covenant transactions would have entered the UTXO set → Utreexo commitment would encode invalid state → network-wide propagation of consensus bugs.

---

## Utreexo in the Layer Model

### Layer Placement

```
┌─────────────────────────────────────────────────┐
│  Layer 0: Consensus (WHAT is valid)             │
│  - Taproot validation                           │
│  - Covenant enforcement ← FIXED in Phase L0     │
│  - Script verification                          │
│  - Transaction rules                            │
└────────────────┬────────────────────────────────┘
                 │
                 ↓ DEPENDS ON (must be correct)
                 │
┌─────────────────────────────────────────────────┐
│  Layer 1: State Representation (HOW stored)     │
│  - Utreexo (accumulator)                        │
│  - AssumeUTXO (snapshots)                       │
└─────────────────────────────────────────────────┘
```

**Utreexo is Layer 1** because:
- Changes HOW UTXOs are represented (accumulator vs full set)
- Does NOT change WHAT makes a UTXO valid
- Provides proofs (acceleration), not authority

---

## The Critical Dependency Chain

### How Utreexo Commitment is Calculated

**From DIN-UTREEXO-SPEC.md:**
```
Block N (Height N)
│
├─ 1. Start with UTXO state from Block N-1
├─ 2. Process Block N transactions
│     ├─ Delete spent UTXOs
│     └─ Add new outputs (coinbase + tx outputs)
│           ↑
│           └─── LAYER 0 VALIDATION HAPPENS HERE
│
└─ 3. Header.utreexoCommitment = commitment AFTER step 2
         ↑
         └─── LAYER 1 CALCULATION HAPPENS HERE
```

### The Validation Flow

```cpp
// Step 1: Layer 0 validates block (ConnectBlock)
bool connected = p2p::ConnectBlock(block, utxo_set, error);

// Inside ConnectBlock:
for (tx in block.transactions) {
    // Layer 0: Verify script (including covenant opcodes)
    if (!VerifyScript(tx, utxo_set, SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_COVENANTS)) {
        return false;  // ← Invalid covenant tx REJECTED here
    }

    // Only valid txs reach this point:
    utxo_set.AddOutputs(tx);  // Add to UTXO set
}

// Step 2: Layer 1 calculates commitment from UTXO set
Hash256 commitment = utreexo_accumulator.getCommitment();

// Step 3: Verify block header commitment matches
if (block.header.utreexoCommitment != commitment) {
    return false;  // Commitment mismatch
}
```

### The Dependency

**Utreexo MUST trust that**:
1. Layer 0 correctly validated all transactions before adding outputs
2. UTXO set contains ONLY valid outputs
3. Invalid covenant transactions were rejected

**If Layer 0 is broken** (as it was before Phase L0):
- Invalid covenant tx → accepted by Layer 0
- Invalid output → added to UTXO set
- Utreexo commitment → calculated from corrupted state
- **Network propagates invalid commitment**

---

## What Happens With Layer 0 Broken (Pre-Phase L0)

### Attack Scenario: Invalid CTV in Utreexo Commitment

**Step-by-step breakdown:**

```
1. Attacker mines block with invalid OP_CHECKTEMPLATEVERIFY transaction
   └─ CTV template hash is WRONG (doesn't match tx structure)

2. Layer 0 validates block (PRE-FIX):
   ├─ Block validation calls VerifyScript()
   ├─ But covenant flags NOT passed to script verification
   └─ Invalid CTV treated as NOP → transaction ACCEPTED ✗

3. Invalid transaction's outputs added to UTXO set:
   ├─ UTXO: {txid: "abc...", vout: 0, amount: 100, spk: "..."}
   └─ This UTXO is INVALID (came from invalid covenant tx)

4. Utreexo commitment calculated:
   ├─ leaf_hash = SHA256(txid || vout || amount || spk)
   ├─ Merkle forest updated with invalid leaf
   └─ commitment = SHA256(forest_roots)

5. Commitment added to block header:
   └─ header.utreexoCommitment = commitment (encodes invalid state)

6. Block propagates to network:
   ├─ Other nodes validate block
   ├─ Layer 0 broken on all nodes → accept invalid tx
   ├─ All nodes calculate SAME invalid commitment
   └─ Block accepted network-wide ✗

7. Pruned nodes use Utreexo proofs:
   ├─ Request proof for spending this UTXO
   ├─ Proof validates against invalid commitment
   └─ Pruned nodes accept invalid state ✗

8. Result:
   └─ Network-wide consensus on INVALID state
```

### Why This is Catastrophic

**Utreexo amplifies Layer 0 bugs**:
- Layer 0 bug: Invalid tx accepted
- Layer 1 encoding: Invalid state committed to block header
- Layer 1 proofs: Invalid state propagated to pruned nodes
- **Permanent**: Once in commitment, hard to rollback

**Recovery is extremely difficult**:
- Can't just "fix the code" - invalid state already in blockchain
- Would need hard fork to invalidate blocks
- Utreexo commitments are part of block hash → changes invalidate chain
- All nodes must coordinate rollback

---

## What Happens With Layer 0 Fixed (Post-Phase L0)

### Same Attack Scenario: Now Prevented

```
1. Attacker mines block with invalid OP_CHECKTEMPLATEVERIFY transaction
   └─ CTV template hash is WRONG

2. Layer 0 validates block (POST-FIX):
   ├─ Block validation calls VerifyScript()
   ├─ Passes SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_COVENANTS ✓
   ├─ Tapscript interpreter encounters OP_CHECKTEMPLATEVERIFY
   ├─ OpCheckTemplateVerify() called
   ├─ Verifies template hash against tx structure
   └─ Hash mismatch → VerifyScript returns FALSE ✓

3. Block rejected:
   ├─ ConnectBlock() returns false
   ├─ Invalid tx NOT added to UTXO set ✓
   └─ Block NOT accepted ✓

4. Utreexo commitment never calculated:
   └─ Block rejected before commitment calculation ✓

5. Network protected:
   ├─ Invalid block NOT propagated
   ├─ Utreexo commitment stays valid
   └─ No invalid state encoded ✓
```

### Why This Works

**Layer 0 now enforces covenant rules BEFORE Layer 1**:
1. Invalid covenant tx → rejected at Layer 0
2. UTXO set contains ONLY valid outputs
3. Utreexo commitment encodes ONLY valid state
4. Proofs validate against valid commitments

**Separation of concerns maintained**:
- Layer 0: Decides WHAT is valid
- Layer 1: Encodes valid state efficiently
- Layer 1 never has to worry about invalid state (Layer 0 prevents it)

---

## Utreexo's Layering is Correct

### Does Utreexo Change WHAT is Valid? ❌ NO

**Evidence**:

1. **Commitment is DERIVED from state, not source of truth**
   ```cpp
   // Commitment calculated AFTER applying Layer 0 rules
   simulated = chainstate.clone()
   simulated.applyBlock(BlockN)  // ← Layer 0 validation
   commitment = simulated.getCommitment()  // ← Layer 1 calculation
   ```

2. **Can disable Utreexo without changing consensus**
   - Archive nodes: Maintain full UTXO set, ignore commitments
   - Pruned nodes: Use Utreexo proofs as optimization
   - Consensus rules: Identical for both node types

3. **Proofs are untrusted acceleration**
   - Inclusion proof must be verified against commitment
   - Invalid proof → rejected
   - Proof doesn't define validity, just proves membership

### Does Utreexo Change HOW State is Represented? ✅ YES

**Evidence**:

1. **Storage model changed**
   - Archive nodes: Store all UTXOs (~GB)
   - Pruned nodes: Store accumulator roots (~KB)
   - Same validity rules, different storage

2. **Proof requirement changed**
   - Archive nodes: No proofs needed (have full set)
   - Pruned nodes: Need inclusion proofs
   - Proofs are Layer 1 optimization, not Layer 0 requirement

3. **Commitment in header**
   - Enables efficient state validation
   - Does not replace transaction validation
   - Derived from UTXO state, not the other way around

**Verdict**: ✅ Utreexo is correctly layered (Layer 1)

---

## Why Layer 0 Fixes Were Critical for Utreexo

### Before Phase L0

| Issue | Impact on Utreexo |
|-------|-------------------|
| Covenant opcodes not enforced | Invalid covenant txs → UTXO set → commitment encodes invalid state |
| SCRIPT_VERIFY_STANDARD missing flags | Even if ConnectBlock called, wouldn't enforce covenants |
| Tapscript interpreter missing handlers | Covenant opcodes treated as unknown → accepted |
| Mempool validation broken | Invalid txs propagate → mined → committed |

**Result**: Utreexo would commit to invalid UTXO state

### After Phase L0

| Fix | Benefit for Utreexo |
|-----|---------------------|
| Block validation enforces covenants | Invalid covenant txs rejected before UTXO add |
| SCRIPT_VERIFY_STANDARD includes flags | ConnectBlock enforces all covenant rules |
| Tapscript interpreter handles covenants | Covenant opcodes properly validated |
| Mempool validation matches blocks | Invalid txs rejected before mining |

**Result**: Utreexo commitments encode ONLY valid UTXO state

---

## Utreexo ↔ AssumeUTXO Interaction

Both are Layer 1, but serve different purposes:

### Utreexo (Ongoing Optimization)
- **Purpose**: Reduce UTXO set storage requirement
- **When**: Every block (ongoing)
- **How**: Merkle accumulator + inclusion proofs
- **Dependency**: Layer 0 validates txs before accumulator update

### AssumeUTXO (Bootstrap Optimization)
- **Purpose**: Fast initial sync
- **When**: Once (during initial sync)
- **How**: Snapshot + background validation
- **Dependency**: Layer 0 validates blocks during background sync

### Combined Usage

```
New node joins network:
│
├─ 1. Load AssumeUTXO snapshot (instant wallet)
│     ├─ Assumed UTXO set loaded
│     └─ Background validation starts (uses Layer 0)
│
├─ 2. Sync new blocks with Utreexo proofs
│     ├─ Receive blocks with commitments
│     ├─ Verify inclusion proofs against commitments
│     └─ Update accumulator (no full UTXO set needed)
│
└─ 3. Background validation completes
      ├─ AssumeUTXO validated from genesis
      └─ Switch to validated chainstate
```

**Both depend on Layer 0 being correct**:
- AssumeUTXO: Background validation uses ConnectBlock (Layer 0)
- Utreexo: Commitments calculated after ConnectBlock (Layer 0)

---

## Comparison: Utreexo vs Traditional UTXO Set

| Aspect | Traditional (Archive) | Utreexo (Pruned) |
|--------|----------------------|------------------|
| **Storage** | Full UTXO set (~GB) | Accumulator roots (~KB) |
| **Validation** | Direct UTXO lookup | Verify inclusion proof |
| **Consensus Rules** | Layer 0 (identical) | Layer 0 (identical) |
| **Layer Classification** | N/A (baseline) | Layer 1 (optimization) |
| **Can Disable?** | N/A (required) | Yes (fall back to archive) |
| **Depends on Layer 0?** | Yes | Yes |
| **Changes WHAT is valid?** | No | No |
| **Changes HOW stored?** | Baseline | Yes |

**Key Insight**: Utreexo and traditional UTXO set use **identical Layer 0 validation** - they just store the result differently.

---

## Utreexo Security Properties

### What Utreexo Does NOT Protect Against

❌ **Invalid transactions** - This is Layer 0's job
❌ **Invalid covenant opcodes** - This is Layer 0's job
❌ **Script verification bypasses** - This is Layer 0's job

**Why**: Utreexo operates AFTER Layer 0 validation. If Layer 0 is broken, Utreexo cannot detect it.

### What Utreexo DOES Protect Against

✅ **Invalid inclusion proofs** - Proof doesn't match commitment → reject
✅ **Commitment mismatch** - Block header commitment ≠ calculated commitment → reject
✅ **Storage corruption** - Accumulator roots protect UTXO set integrity

**Why**: These are Layer 1 concerns (state representation integrity)

### The Trust Model

**Utreexo user trusts**:
1. Layer 0 validation is correct (now fixed ✓)
2. Block header commitments are honest (validated by miners/nodes)
3. Inclusion proofs are honest (validated against commitments)

**Utreexo user does NOT need to trust**:
- The node providing proofs (proofs are verified)
- Historical block data (commitments enable verification)
- Other nodes' UTXO sets (accumulator is self-validating)

---

## Conclusion

### Utreexo's Role

**Utreexo is Layer 1 (State Representation)** because:
- Changes HOW UTXOs are stored (accumulator vs full set)
- Does NOT change WHAT makes a UTXO valid (Layer 0's job)
- Provides proofs as optimization, not authority
- Can be disabled without changing consensus

### Critical Dependency on Layer 0

**Utreexo REQUIRES Layer 0 to be correct** because:
- Commitments calculated AFTER Layer 0 validation
- If Layer 0 accepts invalid txs → Utreexo commits to invalid state
- Layer 1 cannot detect Layer 0 bugs (by design)
- Invalid commitments propagate network-wide

### Why Layer 0 Fixes Were Essential

**Before Phase L0**:
- 🔴 Layer 0 accepted invalid covenant transactions
- 🔴 Utreexo would commit to invalid UTXO state
- 🔴 Network-wide propagation of consensus bugs
- 🚫 **Utreexo NOT safe to deploy**

**After Phase L0**:
- ✅ Layer 0 rejects invalid covenant transactions
- ✅ Utreexo commits to ONLY valid UTXO state
- ✅ Consensus bugs prevented at source
- ✅ **Utreexo safe to deploy**

### Architectural Compliance

✅ **Layer boundaries respected** - Utreexo is Layer 1, depends on Layer 0
✅ **Invariants satisfied** - Utreexo doesn't weaken Layer 0
✅ **Independence maintained** - Can disable Utreexo, consensus unchanged
✅ **Trust model correct** - Proofs verified, not trusted

### Architectural Invariant

**"Utreexo commitments MUST be computed exclusively from consensus-validated UTXO state."**

This invariant ensures:
- Commitments are calculated AFTER Layer 0 validation completes
- Invalid transactions are rejected before entering the UTXO set
- Utreexo never commits to consensus-invalid state
- Layer boundaries are preserved (Layer 1 depends on Layer 0, not vice versa)

---

**Summary**: Utreexo is a correctly-layered Layer 1 feature that depends on Layer 0 being correct. The Phase L0 fixes were critical because they ensure Utreexo commitments encode only valid UTXO state, preventing network-wide propagation of consensus bugs.

---

**Document Date**: 2025-12-24
**Related Documents**:
- `LAYER_0_AUDIT_FINAL.md` - Layer 0 status
- `LAYER_1_CAPABILITY_AUDIT.md` - Layer 1 status
- `DIN-UTREEXO-SPEC.md` - Utreexo specification
