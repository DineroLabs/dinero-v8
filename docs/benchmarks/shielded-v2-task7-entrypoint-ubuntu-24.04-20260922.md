# Shielded v2 Task 7 entry-point bench on the GitHub x86 runner (2026-09-22)

Workflow `shielded-v2-spike-bench` run 35688016290 on commit 27087a7d16782236bb84c7f3c767fa31cbc1bcf6
(claude/shielded-v2). Host: ubuntu-24.04 Azure VM, AMD EPYC 7763, **2 physical cores / 4 vCPUs**,
16 GB. Raw rows: `shielded-v2-task7-entrypoint-ubuntu-24.04-20260922.json`. All checks passed on
this host: SpartanProfileCompat 6/6, self-test, negatives 0 failures, profile soundness 0 failures.

| measurement (2-in-2-out, fresh proofs) | M4 Max (8 threads) | this runner |
|---|---|---|
| warm entry point, matrix budget 8 threads | 18.8 ms | 61.1 ms (only 4 vCPUs exist) |
| warm entry point, serial | 33.8 ms | 86.8 ms |
| cold init per shape (build + hash) | 50.9 ms | 91.4 ms |
| 50 proofs, 8 workers, serial inner | 263 ms | 1,901 ms |
| prove | 486 ms | 1,323 ms |
| proof bytes | 11,771 | 11,771 |

Reading: per core this VM is ~2.6× slower than the M4 Max (serial verify 80.9 vs 31.4 ms) and it has
a quarter of the cores, so **neither the 20 ms single-verify gate nor the 400 ms block gate is met on
this runner class**. The spec's verify target names "fleet 8-core x86"; this VM is not that class.
A measurement on an idle 8-core x86 host of the fleet's type (not a live mainnet node) is still
required before the gates can be called met on x86. The remaining lever if that measurement also
misses is the O(nnz) matrix walk (80% of the serial verify here): a per-shape compressed sparse
representation or a sparse-polynomial commitment.
