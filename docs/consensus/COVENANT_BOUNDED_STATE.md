# CCV bounded-state analysis

Assurance artifact for release gate 1 of the covenant activation
(`COVENANT_MAINNET_ACTIVATION_100000.md`), specifically the *bounded-state*
evidence.

Companion artifacts: `COVENANT_MUTATION_COVERAGE.md`,
`COVENANT_SANITIZER_FUZZ.md`, `tests/consensus/test_ccv_reference_model.cpp`,
`tests/vectors/bip119_ctvhash.json`.

Implementation: `tests/consensus/test_ccv_bounded_state.cpp` (ctest
`CcvBoundedState`, runs in the normal CI lane, ~15 ms).

## What makes this different from the other covenant tests

| Artifact | Method | What a pass tells you |
|---|---|---|
| Adversarial suite | Specific chosen attacks | The attacks someone thought of are rejected |
| Sanitizer-fuzz | Random sampling | The points it happened to visit were fine |
| Reference model | Differential vs. a spec-derived implementation | Two implementations agree on the sampled points |
| **Bounded state** | **Exhaustive enumeration over reduced domains** | **No surviving counterexample inside the domain** |

The verdict at every enumerated point is **predicted from the specification
first**, then compared against production. This is a decision table, not an
observation: production is not consulted to decide what the answer should be.

## The honest part: what a reduction claims

Enumerating counters over `{0, 1, 2, MAX-2, MAX-1, MAX}` is **not** a proof about
all 2^32 counters. It is a proof about those six values, plus an argued claim
that the rule depends on the counter only through `prev == UINT32_MAX` and
`next == prev + 1` — never on magnitude.

That claim is inspectable in `VerifyContractTransition`, and it is written down
so a reviewer can check it rather than take it on trust. **If production ever
grew a magnitude-dependent rule — a minimum counter, a per-range policy — this
reduction would silently stop being faithful while continuing to pass.** That is
the standing risk of the technique, and it is recorded here deliberately rather
than left as an unstated assumption.

Each dimension below therefore states three things: the production property it
models, the reduction applied, and the assumption that makes the reduction sound.

## Dimensions

### 1. Counter transitions — 36 points

- **Property:** `prevState.counter != UINT32_MAX && newState.counter == prevState.counter + 1`
- **Reduction:** all ordered pairs from `{0, 1, 2, MAX-2, MAX-1, MAX}`
- **Soundness:** the rule reads the counter only via an equality against `MAX`
  and a successor relation. It never compares magnitudes, so behaviour is
  uniform across the interior and can change only at the wrap boundary. Both
  ends are covered, including the `(MAX, 0)` pair a wrap would produce.

### 2. State data sizes — 49 points

- **Property:** both `data` fields bounded by `MAX_CONTRACT_STATE_DATA_SIZE` (448)
- **Reduction:** all ordered pairs from `{0, 1, 2, 446, 447, 448, 449}`
- **Soundness:** the bound is a single `>` comparison against one constant, so
  behaviour can change only at 448/449. Both sides are enumerated in both
  positions — an off-by-one rejecting exactly 448 would break every legitimate
  full-size contract, and is caught here rather than in production.

### 3. Successor placement — 56 points

- **Property:** `tx.vout[inputIndex]` is the successor
- **Reduction:** output vectors of length 1..4 × CCV index 0..3 × successor at
  every position `0..output_count` inclusive, where `position == output_count`
  encodes "successor absent". The point count is derived in the test rather than
  hardcoded, so it cannot drift from the loops.
- **Soundness:** the index rule is a direct subscript comparison, structural in
  the output vector rather than dependent on its absolute length. Longer vectors
  add no new branches.

### 4. Duplicate successors — 10 points

- **Property:** no output other than `tx.vout[inputIndex]` may carry the
  successor script
- **Reduction:** vectors of length 2..5, successor at index 0, duplicate planted
  at every other position
- **Soundness:** detection is a full linear scan, so it cannot be
  position-sensitive; every position within short vectors covers the adjacent
  and distant cases alike.

### 5. Confidentiality — 4 points, genuinely exhaustive

- **Property:** spent output and successor must both be transparent
- **Reduction:** none. The complete 2×2 cross-product.
- **Rationale:** V1 rejects confidential CCV because equality of Pedersen
  commitments is not a proof of value equality when blinders may differ.

### 6. Value relation — 6 points

- **Property:** the successor preserves the spent value exactly
- **Reduction:** `{0, spent-2, spent-1, spent, spent+1, spent+2}`; exactly one
  point may be accepted
- **Soundness:** a single equality comparison cannot be sensitive to the
  magnitude of a difference, only to whether one exists.

## Anti-vacuity

Every dimension asserts that **both** accepted and rejected outcomes occur. A
verifier that answered the same way everywhere would otherwise satisfy
"production agreed at every enumerated point" while proving nothing.

## Reproducing

```sh
cmake -S . -B build
cmake --build build --target test_ccv_bounded_state -j8
ctest --test-dir build -R '^CcvBoundedState$' --output-on-failure --no-tests=error
```

## Neuter proof

Removing `newState.counter != prevState.counter + 1` from
`VerifyContractTransition` and running **only** this suite — so the proof is not
borrowed from the adversarial or differential tests — fails it, naming every
offending point:

```
prev=0 next=0            expected false, got true   (replay)
prev=0 next=2            expected false, got true   (skip)
prev=0 next=4294967293   expected false, got true
```

That is the practical advantage of enumeration over sampling: the failure output
is the complete set of violating transitions, not a single example.

`covenants.cpp` restored byte-identical afterwards
(sha256 `2b8b4d1266244bba22e9688dc4e5eacf3ecc517354c3a4be9d28353287e1b49c`).

This suite is also registered in `tools/covenant_mutation_harness.py`, so it
participates in the standing mutation score rather than being scored only once
by hand.

## Deliberate limits

- Reduced domains are not full domains. Every claim above is bounded by its
  stated soundness assumption; none of them is a proof over all inputs.
- Dimensions are enumerated **independently**, not as a full cross-product.
  A defect requiring, say, a specific counter *and* a specific output arity
  simultaneously would not be reached. Full cross-product is combinatorially
  infeasible and was not attempted.
- Only `VerifyContractTransition` is enumerated. The wire decoder, activation
  heights, relay policy, and mempool re-validation are covered by other
  artifacts.
