# Shielded v2 spike report — 2026-09-22

## Machine and commits
Apple M4 Max, Release. dinero-v8 worktree `claude/shielded-v2` @ `3a29bcef5` (harness, circuit, JSON). WHIR rev `c03a4a5`. Data: `docs/benchmarks/shielded-v2-spike-20260922.json`, `docs/benchmarks/shielded-v2-spike-phase2-20260922.md`.

## Phase 1 — bundle circuit on the existing Spartan+Hyrax
| | constraints | proof bytes | prove ms | verify ms | target (spec §2) | pass? |
|---|---|---|---|---|---|---|
| baseline: ONE cv-bound spend proof (today) | ~438k | 44,571 | 3,220 | 854 | — | — |
| today's 2-in-2-out tx (4 proofs) | ~1.75M | ~178,000 | ~12,900 | ~3,400 | — | — |
| bundle 1-in-2-out | 29,121 | 12,364 | 281 | 39 | 16 KB / 300 ms / 10 ms | size ✓, prove ✓, verify ✗ |
| bundle 2-in-2-out | 53,038 | 18,792 | 486 | 65 | 16 KB / 300 ms / 10 ms | ✗ (1.2×) / ✗ (1.6×) / ✗ (6.5×) |
| bundle 4-in-2-out | 100,872 | 18,488 | 852 | 118 | — | — |
| 50 × 2-in-2-out verify | — | — | — | 3,960 sequential / 583 on 8 threads | 200 ms | ✗ (2.9×) |

Correctness: honest bundles satisfied; wrong balance/fee/anchor/nullifier/65-bit value/commitment all unsatisfied; neutering the balance constraint makes the fee check pass (restored); flipping a public input makes the verifier reject.

## Phase 2 — hash-based PCS (WHIR)
See the phase-2 file. Per opening at 2^16, rate 3: 79.6 KB, 21 ms prove, **0.48 ms verify**. Two openings per tx ≈ 150–160 KB non-ZK. ZK mode as implemented: 1.16 MB — not usable.

## Findings
1. The cv-bound Pedersen commitment is the entire cost problem: removing it and proving balance in-circuit cuts a 2-in-2-out transaction from ~1.75M to 53k constraints (33×), proof bytes −89%, prove −96%, verify −98%, with zero new dependencies and the note format unchanged.
2. Phase 1 misses the owner's tightened gates by 1.2× (size), 1.6× (prove) and 6.5× (verify, 65 ms). The verify floor is Hyrax's ~1.4k curve operations; batching across 8 threads gives 11.7 ms/tx amortized.
3. A hash-based PCS meets the verification target with a large margin (sub-millisecond per opening), is post-quantum and transparent, but costs ~8× the bytes of Hyrax; two openings land at ~150 KB, 2.4× the 64 KB target. Dropping the `E` opening halves that.
4. Zero knowledge with a hash-based PCS is the open design problem: the off-the-shelf zk mode is unusable; a masked-sum-check construction must be designed and measured before phase 2 can be scheduled.
5. The 16 KB / 10 ms / 300 ms gates were set before any measurement; phase 1 as measured is a 25–50× improvement on every axis and phase 2 trades bytes for verification speed and PQ.

## Go / no-go
- **Phase 1: GO.** Implement the bundle proof on the existing prover behind the `DZV2` envelope (id 0x01). Ship it against relaxed, measured gates: ≤ 20 KB, ≤ 70 ms single-verify, ≤ 12 ms/tx amortized in a block, ≤ 500 ms prove on M4 Max; then tune (drop `E` opening; Hyrax column shaping) toward the original numbers.
- **Phase 2: GO for design, DEFER for implementation.** Choose WHIR (Goldilocks3, Blake3, rate 3–4) as the PCS; write and measure the ZK masking design first; re-baseline the size target at ≤ 100 KB per tx (one opening + masks), keeping ≤ 10 ms verify.

## What the phase-1 plan must contain
Envelope `DZV2|id`, bundle statement exactly as `tests/spike/bundle_circuit_v2.h` (with the auth-profile nullifier key), transcript domain `dinero.shielded.bundle.v2` binding the sighash, consensus vectors + neuter tests from spec §6, the Release performance ctest at the relaxed gates above, batched verification off the ingress lock, activation/sunset heights at `UINT32_MAX`, wallet/DPI prover through the existing C++ API first (FFI crate only in phase 2).
