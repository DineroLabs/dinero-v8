# Shielded v2: bundle proofs, hash-based commitment, post-quantum verification

Status: design approved by the owner 2026-09-22 (chat); not implemented. Branch `claude/shielded-v2`, worktree `~/src/dinero-v8-shielded-v2`. Nothing here merges to `dinero-main` until the gates in §6 pass and the owner approves.

## 1. Problem

Shielded proofs cost 40–170 KB per transaction and 1–9 s to verify (M4 Max, `docs/benchmarks/shielded-cost-20260915.json`); Spartan proof bytes are 93–97% of a shielded transaction. The cause is not Spartan: the spend circuit is ~8,400 constraints and the output circuit ~480 (`include/consensus/shielded/shielded_circuit.h:19,33`). The cv-binding profile, active on mainnet since height 61000 (`src/consensus/chainparams_impl.cpp:121`), adds ~430,000 constraints per proof to compute the Pedersen value commitment `cv = rcv·G + value·V` on secp256k1 inside R1CS. The Hyrax polynomial commitment is discrete-log based, so the pool is not post-quantum despite the project's positioning. Codex's compact encoding (#796, dormant) shrinks bytes ~44% but does not change proving or verification cost.

## 2. Requirements (owner decisions)

- Post-quantum and no trusted setup: hash-based commitments only in the final system.
- Optimize verification cost first; proof size and proving time second.
- Coexist with v1 by proof version; the note format, commitment tree, nullifier derivation, wallet scanning and migration tooling do not change.
- Targets (Release builds; gates, not aspirations):

| metric | target | gate threshold in CI (2× target) |
|---|---|---|
| verify, one 2-in-2-out tx | < 10 ms (fleet 8-core x86) | 20 ms |
| verify, block of 50 shielded txs, batched | < 200 ms | 400 ms |
| prove 2-in-2-out, iPhone-class | < 1 s | 2 s |
| prove 2-in-2-out, M4 Max | < 300 ms | 600 ms |
| proof bytes per tx, phase 1 | < 16 KB | 32 KB |
| proof bytes per tx, phase 2 | < 64 KB | 128 KB |

## 3. Architecture

### 3.1 One bundle proof per transaction
Replace per-spend and per-output proofs with a single proof over the whole shielded bundle.

Statement (all hashes Poseidon-2 over the secp256k1 scalar field, as today):
- For each spend i: `public_key = H(secret_key, 0)`; `addr_bind = H(ADDR_TAG, H(d, public_key))`; `commitment = H(H(addr_bind, value_i), randomness)`; Merkle path (depth 32) from `commitment` to a published `anchor_i`; `nullifier_i = H(nullifier_key, leaf_index)` using the auth-profile key derivation active since height 110000; `value_i` is 64-bit.
- For each output j: `addr_bind`, `commitment_j` as today; `value_j` is 64-bit.
- Balance: `Σ value_i = Σ value_j + fee`, `fee` public and 64-bit.
- Binding: the proof's transcript absorbs `sighash` (domain `DIN/v7/shielded/v2/bundle-sighash`), so a proof cannot be moved to another transaction.

Public inputs: `anchor[]`, `nullifier[]`, `commitment[]`, `fee`, `sighash`, spend and output counts. Removed: `cv`, `rcv`, the binding signature, the Bulletproofs range-proof FFI. Constraint budget: ≈ 8.4k per spend + 0.5k per output + ~200 for balance/range/binding. A 4-in-2-out bundle is ≈ 35k constraints (v1: 6 proofs × 430k).

### 3.2 Envelope
`ShieldedBundle.v2_proof` = `tag "DZV2" | proof_system_id (u8) | length | bytes`. `proof_system_id` 0x01 = Spartan + Hyrax (phase 1), 0x02 = Spartan + hash-based PCS (phase 2). Unknown ids fail closed. The compact DZE1 tag and v1 fields stay untouched.

### 3.3 Two phases, one circuit
- Phase 1: the existing native C++ Spartan (`src/consensus/shielded/shielded_circuit.cpp`, sum-check + Hyrax) proving the bundle circuit. No new dependency. Expected ≈ 40× fewer constraints than today's per-tx total; discrete-log security (interim, not post-quantum).
- Phase 2: same sum-check core, polynomial commitment replaced by a hash-based scheme (Brakedown / Basefold family; candidates and selection in §7). Delivered as a Rust crate with a C FFI in the pattern of `third_party/bulletproofs_ffi`. Activates by height by allowing id 0x02; id 0x01 sunsets later.

### 3.4 Verification path
Block validation collects all v2 proofs of a block and verifies them in a parallel batch on a worker pool, **outside** `g_block_ingress_mutex` and `activation_mutex_` (see #799/#803 for why a slow verifier under those locks starves the node). Results feed the existing `VerifiedProofCache`. Mempool admission verifies single proofs the same way, with the cache.

### 3.5 Coexistence
From `shielded_v2_activation_height`, a transaction carries v1 or v2 proofs (never both). From `shielded_v1_sunset_height`, new blocks may not contain v1 proofs. Historical blocks verify with the preserved v1 profiles. Both heights are `UINT32_MAX` until set by the owner after §6; regtest override via the existing CLI mechanism only.

## 4. Prover and wallets
One Rust crate (`third_party/shielded_v2_ffi`, phase 2; phase 1 uses the C++ prover directly) exposed over C to dinerod, dinero-qt and NodeCore/DineroDPI. API: `prove_bundle(witness) -> proof`, `verify_bundle(public_inputs, proof) -> bool`, `batch_verify(...)`. Wallets build the same `SpendWitness`/output structs as today minus `rcv`.

## 5. Threat notes
- Fee as a public input reveals nothing new (fees are already visible in the transaction).
- Sighash binding replaces the binding signature's anti-replay role; the transcript domain tag and the exact sighash preimage are consensus-critical and get an independent review before activation.
- Batch verification must reject the whole batch on any failure and then re-verify individually to attribute the failing transaction (DoS shaping: a peer cannot make one bad proof invalidate a block cheaply).
- The verifier is reachable by any peer; the proof size cap and constraint-count cap are enforced before verification.

## 6. Testing and qualification (gates)
1. Consensus vectors, fixed bytes with exact txid/sighash, in a path-filtered CI lane: valid 1-in-2-out / 2-in-2-out / 4-in-2-out; wrong balance; wrong anchor; reused nullifier; wrong sighash binding; v1 proof after sunset; v2 proof before activation; unknown proof_system_id.
2. Neuter tests: removing the balance constraint, a range check, or the sighash absorption must fail the corresponding vector.
3. Performance ctest (Release only) asserting the §2 gate thresholds on the CI runner class; failure blocks merge.
4. Sanitizers: ASan and UBSan lanes over the v2 codec and verifier (ASan currently "unqualified" on the compact codec; this design makes it a gate). libFuzzer harness over the envelope decoder and verifier inputs.
5. Regtest harness (extends Codex's combined harness): activate v2, spend v1 notes with v2 proofs, reorg across the activation boundary, restart, apply the v1 sunset, replay.
6. The 22 shielded end-to-end ctests listed in `scripts/ci/unexecuted_tests_baseline.txt` are wired into a lane as part of phase 1.
7. Every fix or rule ships with a test that fails without it (repo rule).

## 7. Spike (first deliverable, throwaway code)
1. Implement the bundle statement in the existing C++ R1CS gadgets; measure constraint count, proof bytes, prove ms, verify ms for 1-in-2-out, 2-in-2-out, 4-in-2-out with the current Spartan+Hyrax on the M4 Max. This is the phase-1 estimate.
2. Run the same circuit through two candidate hash-based commitment stacks in Rust and measure the same table. Candidates to evaluate (final choice by measurement): Microsoft Spartan2 with a Brakedown/Ligero-style PCS; a Basefold/FRI-style PCS over the same field; Binius if field conversion cost is acceptable. Selection criteria: verify time first, proof bytes second, code maturity and audit history third, license compatible.
3. Report: measured table, go/no-go per phase, and the concrete library choice. Nothing from the spike is merged.

## 8. Rollout
Spike → phase 1 on regtest → private canary (two-node) → Release-binary canary on SJ with the pool mining shielded test traffic for 24 h → NA → EU1 → owner sets `shielded_v2_activation_height` → v1 sunset two weeks after activation. Phase 2 repeats the same path; only `proof_system_id 0x02` is enabled. Independent of the 60-second and compact activations.

## 9. Ownership
Claude owns this design, the spike, the phase-1 implementation and the qualification harness on `claude/shielded-v2`. Consensus-statement review (§3.1, §5) is independent. Owner decides activation heights and merges.
