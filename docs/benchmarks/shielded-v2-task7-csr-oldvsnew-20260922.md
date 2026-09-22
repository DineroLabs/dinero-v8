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

| | proof bytes | prove | verify |
|---|---|---|---|
| old: cv-bound spend proof (mainnet profile 61000–109999) | 44,571 | 3,220 ms | 864 ms |
| old: cv-bound output proof (live profile for outputs) | 43,185 | 3,156 ms | 826 ms |
| old per tx, lower bound = 2 spends + 2 outputs | ≈ 175 KB | ≈ 12.8 s | ≈ 3.4 s |
| new: one bundle proof, serial | 11,771 | 483 ms | 28.4 ms |
| new: one bundle proof, 8 matrix threads | 11,771 | 483 ms | 18.1 ms |

The old spend row is the cv-bound profile because the bench can build a valid witness for it; the
LIVE spend profile (Auth) is 1,045,170 constraints against 597,009, so the true old per-tx cost is
higher than the lower bound above. Old proofs are verified one at a time by the existing verifier.

## Worker sweep, 50 fresh 2-in-2-out proofs, one budget, serial inner path

| workers | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| wall ms | 1,493 | 750 | 400 | 223 |
| per proof amortized | 29.9 | 15.0 | 8.0 | 4.5 |

Scaling is near-linear to 8 workers on this host. The same sweep on the GitHub 2-core VM is produced
by the `shielded-v2-spike-bench` workflow on every push of the research branch.

## What this does and does not show

It shows the proof stage only, on an idle machine, with cold init excluded (52 ms once per shape).
It does not show block-validation latency, CPU utilization, catch-up, forks, template/RPC service
under load, or the cost of rejecting invalid proofs; those need the integrated node. The 20 ms and
400 ms thresholds and their hardware contract remain the owner's to fix (plan Global Constraints).
