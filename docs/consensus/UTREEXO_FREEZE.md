# Utreexo Consensus Freeze

**Freeze Commit**: `06cff1ad2` (2026-01-31)
**Status**: LOCKED - Do not modify without hard-fork review

---

## Consensus-Critical Rules

### 1. Canonical Ordering

```
REMOVE ALL spent UTXOs → ADD ALL new UTXOs
```

Never interleaved per-transaction. Block-level batch only.

### 2. Proof Timing

Proofs are generated against `root_before` (pre-block state).
Verification must use the root at block N-1 to validate block N.

### 3. Stateless Equivalence Invariant

**Core guarantee**: Given identical inputs, both paths produce identical roots.

```
Path A (Assembler):     forest.remove(spends) → forest.add(outputs) → root_r1
Path B (Stateless):     proof.verify(targets, positions) → apply delta → root_r1

INVARIANT: root_r1(A) == root_r1(B)
```

This is tested in `test_stateless_proof_minimal.cpp` Test 7.

### 4. BlockUtreexoProof Structure

```cpp
struct BlockUtreexoProof {
    vector<Hash>    targets;      // Leaf hashes (spent UTXOs)
    vector<uint64>  positions;    // Leaf positions (same order as targets)
    vector<Hash>    proof_hashes; // Merkle authentication path
    uint64          numLeaves;    // Forest size at proof generation
};
```

`positions.size() == targets.size()` - always.

---

## Policy vs Consensus

| Constant | Value | Type | Can Change? |
|----------|-------|------|-------------|
| MAX_UTREEXO_LEAVES | 2^40 | Policy | Yes (softfork) |
| MAX_PROOF_DICTIONARY_SIZE | 10,000 | Policy | Yes |
| MAX_PROOF_HASHES | 10,000 | Policy | Yes |
| MAX_PROOF_TARGETS | 100,000 | Policy | Yes |
| MAX_UTREEXO_PROOF_BYTES | 4 MB | Policy | Yes |
| MAX_TREE_HEIGHT | 40 | Derived | Follows MAX_UTREEXO_LEAVES |

**Policy bounds** reject malformed/oversized input before allocation.
They do NOT affect proof validity or consensus rules.

---

## Consensus Gate (CI Must Enforce)

**43 tests across 3 gates. All must pass. No exceptions.**

### Gate 1: Utreexo Accumulator (13 tests)
| Test | File | What It Locks |
|------|------|---------------|
| Tests 1-7 | `test_stateless_proof_minimal.cpp` | Stateless verification |
| Test 7 | Same | Assembler ↔ Stateless equivalence |
| Tests 8-10 | Same | Compression v2 validation |
| Tests 11-12 | Same | Overflow protection |
| Test 13 | Same | DoS bounds |

### Gate 2: Block Validation (5 tests)
| Test | File | What It Locks |
|------|------|---------------|
| Test 1 | `test_utreexo_enforcement.cpp` | Wrong root rejection |
| Test 2 | Same | Valid root acceptance |
| Test 3 | Same | Multiple outputs |
| Test 4 | Same | Legacy mode fallback |
| Test 5 | Same | Commitment determinism |

### Gate 3: P2P Adversarial (25 tests)
| Test | File | What It Locks |
|------|------|---------------|
| D1.1-D1.4 | `test_p2p_adversarial.cpp` | Mempool DoS resistance |
| D2.1-D2.3 | Same | Block relay attacks |
| D3.1-D3.2 | Same | IBD ↔ Live equivalence |

**If ANY of these 43 tests fail, the PR cannot be merged.**

---

## Modification Policy

**Refactors, cleanups, or optimizations that alter execution order or data flow are considered consensus changes.**

To modify Utreexo consensus behavior:

1. Requires hard-fork review process
2. Must update this document
3. Must add new freeze commit
4. All 43 consensus gate tests must pass
5. Equivalence invariant must hold

For policy tuning (MAX_* bounds): softfork-safe, update this doc.

---

## References

- MIT Utreexo Paper: https://eprint.iacr.org/2019/611.pdf
- Implementation: `src/consensus/utreexo_accumulator.cpp`
- Header: `include/consensus/utreexo_accumulator.h`
- CI Gate: `.github/workflows/utreexo-consensus-gate.yml`
- Tests:
  - `tests/test_stateless_proof_minimal.cpp` (Gate 1)
  - `tests/consensus/test_utreexo_enforcement.cpp` (Gate 2)
  - `tests/test_p2p_adversarial.cpp` (Gate 3)
