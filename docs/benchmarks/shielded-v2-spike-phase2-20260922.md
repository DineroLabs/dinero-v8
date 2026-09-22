# Shielded v2 spike — phase-2 polynomial-commitment candidates (2026-09-22)

THROWAWAY measurements (spec §7). Machine: Apple M4 Max, Release builds. Reference (phase 1, same machine, `shielded-v2-spike-20260922.json`): bundle 2-in-2-out on Spartan+Hyrax = 53,038 constraints, 18,792 B, prove 486 ms, verify 65 ms.

## Availability (checked 2026-09-22)
| candidate | status | notes |
|---|---|---|
| `spartan2` 0.9.0 / microsoft/Spartan2 → now `microsoft/vega-prover` (MIT, pushed 2026-07-15) | builds, **not a PQ candidate** | depends on `halo2curves`: discrete-log commitments, same class as today's Hyrax |
| **WHIR** (worldfnd/whir, Apache-2.0, pushed 2026-09-08, rev `c03a4a5`) | built, measured below | hash-based multilinear PCS (arkworks; Goldilocks2/3, Field192/256; Blake3/SHA3). PCS only: needs the Spartan sum-check on top |
| binius 0.1.1 / IrreducibleOSS/binius | not measured | repo last pushed 2025-09-09 (stale ~1 year) |
| Plonky3 / stwo (STARK, FRI) | not measured | approach B (AIR rewrite), not chosen by the owner |
| lattice (LaBRADOR/Greyhound) | no library | research-grade |

## WHIR PCS, one opening, 128-bit security, Blake3, Johnson list-decoding
| 2^d (≈ witness size) | rate r | field | proof bytes | prove ms | verify ms | verifier hashes |
|---|---|---|---|---|---|---|
| 16 (2-in-2-out) | 1 | Goldilocks3 | 127,992 | 93.4 | 0.72 | 2,359 |
| 16 | 3 | Goldilocks3 | 79,576 | 21.2 | 0.48 | 1,491 |
| 16 | 3 | Field256 | 120,192 | 76.7 | 0.58 | 1,514 |
| 16 | 4 | Goldilocks3 | 72,344 | 126.3 | 0.45 | 1,405 |
| 17 (4-in-2-out) | 1 | Goldilocks3 | 138,272 | 99.0 | 0.75 | 2,679 |
| 17 | 3 | Goldilocks3 | 85,312 | 97.2 | 0.48 | 1,667 |
| 17 | 3 | Field256 | 125,440 | 61.8 | 0.55 | 1,676 |
| 17 | 4 | Goldilocks3 | 75,936 | 65.6 | 0.46 | 1,515 |

A Spartan proof opens two committed polynomials (witness `W`, error `E`), so a per-transaction estimate is ~2× one opening plus the sum-check transcripts (a few KB): **≈ 150–160 KB at rate 3, verify ≈ 1–2 ms**.

## Zero knowledge
`main --zk` (feature `rs_in_order`), d=16, r=3, Goldilocks3: **proof 1,157.3 KiB**, prover 150.3 ms, verifier 19.0 ms, 3.1k hashes. This repository's zkWHIR construction (witness + blinding commitments) is unusable at these sizes. Shielded proofs require zero knowledge, so phase 2 needs a masking design (Libra-style masked sum-check + salted Merkle leaves, or a small ZK wrapper) before its size can be stated.

## Reading
- Verification: hash-based PCS meets the <10 ms gate with ~20× margin per opening; the sum-check verifier adds O(log n) field ops.
- Size: hash-based proofs are inherently larger than curve-based ones; even non-ZK, two openings exceed the 64 KB phase-2 target by ~2.4×. Eliminating the `E` opening (Spartan's non-relaxed form when `u = 1, E = 0`) halves this.
- Proving is fast (20–130 ms per opening), so phone-side proving is not the constraint.
