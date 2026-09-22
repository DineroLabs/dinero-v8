# Shielded v2 Task 7: CSR matrix walk, old-vs-new on one host, worker sweep (2026-09-22)

Host: M4 Max, Release, `build-spike`. Bench: `tests/spike/shielded_v2_bundle_bench.cpp` (default mode).
Same binary for both walks; `DINERO_NO_CSR=1` selects the constraint walk. Fresh proofs, medians of 5.

## CSR (per-shape compressed sparse row) versus the constraint walk, 2-in-2-out

| complete warm entry point | constraint walk | CSR | saving |
|---|---|---|---|
| serial | 34.1 ms | 28.5 ms | 17% |
| 8-thread matrix budget | 19.0 ms | 18.1 ms | 5% |
| 50 proofs, 8 workers, serial inner | 269 ms | 223–228 ms | 15–17% |

Shape 2-in-2-out: 292,365 nonzeros, of which 135,317 are +1 and 52,494 are −1 (64% skip the field
multiply); CSR build 1.9 ms once per shape. Verdict equality (walk vs CSR, valid and tampered proofs,
both profiles, 1 and 8 threads) is pinned by `SpartanProfileCompat.CsrMatrixWalkGivesTheSameVerdict…`
and `…LargeFixtureParallelBranchIsVerdictIdenticalOnValidAndInvalidProofs` (20,000-constraint
fixture, so the ≥16,384 parallel branch actually runs). A CSR built for another shape is refused.

## Old design versus new on this host (verification of one 2-in-2-out transaction)

Old rows are real proofs with valid witnesses; the Auth spend is built exactly as the wallet builds
it (`DeriveShieldedAccount`, `DeriveDiversifiedSpendKey`, `DeriveDiversifiedNullifierKey`,
`AuthRecipientCommitmentKey`), proven with `ProveSpend(cv_bound=true, spend_auth=true)` and
verified with `VerifySpend`. Medians of 3 after 1 warm-up for the old rows, of 5 for the new.

| | proof bytes | prove | verify |
|---|---|---|---|
| old, historical cv-bound spend (mainnet 61000–109999, no longer produced) | 44,571 | 3,259 ms | 868 ms |
| **old, live Auth spend (mainnet since 110000)** | 73,281 | 4,493 ms | 1,410 ms |
| old, live cv-bound output | 43,185 | 3,146 ms | 828 ms |
| **old per tx today = 2 Auth spends + 2 outputs** | ≈ 233 KB | ≈ 15.3 s | ≈ 4.5 s |
| new: one bundle proof, serial (complete warm entry point) | 11,771 | 489 ms | 28.9 ms |
| new: one bundle proof, 8 matrix threads | 11,771 | 489 ms | 18.2 ms |

Old proofs are verified one at a time by the existing verifier, which is single-threaded per proof.

## Worker sweep, 50 fresh 2-in-2-out proofs, one budget, serial inner path (medians of 5 after 1 warm-up)

| workers | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| wall ms | 1,512 | 765 | 400 | 223 |
| per proof amortized | 30.2 | 15.3 | 8.0 | 4.5 |

Scaling is near-linear to 8 workers on this host (6.8× at 8). The same sweep and the old baselines
run on the GitHub 2-core VM through the `shielded-v2-spike-bench` workflow on every push.

## Trusted verifier context (owner review of 585097e24, point 1)

`R1CSVerifierMatrices` now carries the circuit's structure hash. A verify that receives a context
requires `proof.circuit_hash == context.circuit_hash` and, when the caller supplies an expected
hash, `expected == context.circuit_hash`, before the dimension checks. A context built for a
different circuit with identical dimensions is refused (test `…SameVerdict…`, twin-circuit case).

## Equivalence tests (point 2)

`MixedCoefficientLargeFixtureAndMatrixEvaluationRejection`: 17,000 constraints of the form
(2x + 3y − 5)(7x − y) = z (general, +1 and −1 coefficients all present, parallel branch reached);
walk and CSR at 1 and 8 threads accept the honest proof under both profiles; a proof for circuit A
verified against circuit B (same dimensions, one coefficient changed) is rejected on the walk path
with the expected hash left empty (so the only differing check is the matrix evaluation) and on the
CSR path with B's context identity forged to A's (so binding passes and M̃_B rejects).

## What this does and does not show

It shows the proof stage only, on an idle machine, with cold init excluded (52 ms once per shape).
It does not show block-validation latency, CPU utilization, catch-up, forks, template/RPC service
under load, or the cost of rejecting invalid proofs; those need the integrated node. The 20 ms and
400 ms thresholds and their hardware contract remain the owner's to fix (plan Global Constraints).

## Same commit on the GitHub x86 runner (run 35689416657, ubuntu-24.04, EPYC 7763, 2 cores / 4 vCPUs)

Raw rows: `shielded-v2-task7-csr-ubuntu-24.04-20260922.json`. All checks passed there (compat 8/8,
negatives, profile soundness).

| 2-in-2-out complete warm entry point | walk (b79d876c1) | CSR (585097e24) | saving |
|---|---|---|---|
| serial | 86.8 ms | 71.3 ms | 18% |
| matrix budget 8 (4 vCPUs exist) | 61.1 ms | 55.2 ms | 10% |
| cold init per shape | 91.4 ms | 96.2 ms | (CSR build 4.6 ms) |

| 50 proofs, one budget, serial inner | 1 worker | 2 | 4 | 8 |
|---|---|---|---|---|
| wall ms (single run each on this commit) | 3,867 | 1,954 | 1,573 | 1,576 |

Scaling stops at 4 workers on this host: 2 physical cores plus hyper-threading gives ~2.5× at
best, and 8 workers buys nothing. Amortized per proof at 4 workers: 31 ms. Against the walk's
1,901 ms at 8 workers, CSR saves 17%. The 20 ms / 400 ms thresholds remain unmet on this runner
class; the design's practicality for small nodes is the owner's assessment (proof stage ≈ 2.6% of
a 60-second interval here), to be confirmed by the integrated node under load.

## Old versus new on the GitHub x86 runner (run 35689979725 on 362767b96, same 2-core VM)

Raw rows: `shielded-v2-task7-oldvsnew-ubuntu-24.04-20260922.json`. Checks: compat 9/9, negatives 0,
profile soundness 0. Old rows medians of 3, new rows and sweep medians of 5, all after a warm-up.

| one 2-in-2-out transaction, this VM | bytes | prove | verify |
|---|---|---|---|
| old, live Auth spend, per spend | 73,281 | 10,399 ms | 2,900 ms |
| old, live output, per output | 43,185 | 7,291 ms | 1,725 ms |
| old per tx today (2 + 2) | ≈ 233 KB | ≈ 35 s | ≈ 9.2 s |
| old, historical cv-bound spend (no longer produced) | 44,571 | 7,683 ms | 1,795 ms |
| new bundle, serial complete warm entry point | 11,771 | 1,317 ms | 70.5 ms |
| new bundle, matrix budget (4 vCPUs) | 11,771 | 1,317 ms | 54.4 ms |

Cold init per shape 149 ms (once). Sweep, 50 proofs, medians of 5: 3,815 / 1,932 / 1,567 / 1,567 ms
for 1 / 2 / 4 / 8 workers (2 physical cores; 8 workers gain nothing over 4). On this host the new
design verifies a transaction ~130× faster serially and in ~20× fewer bytes than today's live
proofs; the 20 ms / 400 ms thresholds are still not met here. Proof stage only; integrated-node
measurements under load remain the deciding evidence.

## Owner qualifications on 83f2d69ab, addressed

1. **Whole-transaction measurement, not a sum of medians.** New row `old_tx_2in2out_measured_live(2auth+2out)`:
   two live Auth spend proofs and two live output proofs are built fresh, then all four are verified
   back to back in one timed region, the way a node verifies a v1 bundle. M4 Max, median of 5:
   **4,285 ms, 232,932 proof bytes**, against 28.6 ms serial / 18.2 ms budget for the bundle
   (≈150× serial). The earlier "≈4.5 s" line was the sum of per-proof medians and is superseded.
   The Linux workflow produces the same measured row on the 2-core VM.
2. **Immutable trusted context.** `R1CSVerifierMatrices` has no public mutable state: private
   members, `Build(const R1CS&)` factory, const accessors; the verifier reads it through a const
   pointer. The forged-identity test uses `ForgeIdentityForTests`, compiled only with
   `DINERO_ZK_TEST_HOOKS` (set on the test target, never on the library or daemon).

Still true: the small-runner thresholds are unmet; the next useful work is integrated-node load
testing under an explicit hardware contract; this research stays separate from the CF /
compact-v1 / 60-second release (#797 merged 2026-09-22, activation unset).
