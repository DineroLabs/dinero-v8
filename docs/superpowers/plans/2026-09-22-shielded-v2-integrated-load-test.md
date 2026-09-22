# Shielded v2 Integrated-Node Load Test Plan

**Goal:** Decide the suitability of the v2 bundle proof by measuring node responsiveness under load, old design against new on identical hosts, instead of proof-stage timings.

**Status:** plan + old-design baseline harness (executable today against the current daemon on regtest). The new-design half runs only after the independent review of the §10.2 statement and the E-less profile clears the consensus wiring (Tasks 1–4, 6 of the phase-1 plan). No activation, sunset, reset or migration is implied.

**Spec:** `docs/superpowers/specs/2026-09-22-shielded-v2-design.md` (§2 targets, §3.4 verification path, §10 amendment, §10.7 security scope). Related: `docs/benchmarks/shielded-v2-task7-*` (proof-stage results), owner reviews under `MemoryMD/design/` and `MemoryMD/evidence/`.

## 1. Hardware contract (proposal for the owner's decision)

The spec names two hosts: "fleet 8-core x86" for the §2 verify target and "the CI runner class" for the §6 gate. Measured so far: M4 Max (meets 20 ms / 400 ms) and the GitHub 2-core EPYC VM (71 ms / 1,567 ms). Proposed contract, one line per class, both always reported:

| class | role | host | what is asserted |
|---|---|---|---|
| fleet | the §2 target host | an idle 8-core x86 of the fleet's server type, NOT a live mainnet node | the spec thresholds (or revised ones) as ctest gates |
| small node | floor for operators on 2–4 vCPU VMs | GitHub `ubuntu-24.04` runner (2 cores / 4 vCPUs), or an equivalent VM | responsiveness bounds of §5 only; no proof-stage threshold |
| mobile | §2 prove target | iPhone-class device via the prover kit | ≤ 2 s prove, measured in Task 5's run log |

The load test asserts nothing about proof-stage milliseconds. It asserts the responsiveness bounds in §5, per class. Proof-stage thresholds stay where the spec puts them until the owner revises them on this evidence.

## 2. Workloads (identical for old and new)

All on regtest with the shielded heights forced low, one node under test (NUT) plus one peer node (PEER) that mines and relays, both on the same host class. Transactions are Auth-profile v1 shielded transactions for the old design and version-7 bundle transactions for the new; shapes 1-in-2-out, 2-in-2-out and 4-in-2-out in a 60 / 30 / 10 mix.

| id | scenario | what is driven | duration |
|---|---|---|---|
| W1 steady | blocks and transactions together | PEER mines one block every 10 s containing up to 50 shielded txs; harness submits 5 shielded txs/s to NUT's mempool throughout | 10 min |
| W2 catch-up | recovery after downtime | NUT stopped for 30 blocks of W1-style load on PEER, then restarted; time to tip | until synced + 2 min |
| W3 fork | reorg under load | PEER and a second peer produce a 6-block fork with shielded txs on both sides; NUT switches | until converged + 2 min |
| W4 service | templates and RPC during validation | W1 load plus `getblocktemplate` every 2 s and `getblockcount`/`getrawmempool`/`getshieldedbalance` every 1 s from a separate client | 10 min |
| W5 hostile | expensive invalid proofs | W1 load plus 2 invalid-proof shielded txs/s (proof bytes mutated, everything else valid) submitted over P2P and RPC | 10 min |

Fresh randomness everywhere: no proof is ever seen twice by the NUT, so the verification cache cannot help.

## 3. Metrics (all captured per scenario, per design, per host class)

- Block latency at NUT: time from block arrival (P2P receive or `submitblock` entry) to `ActivateBestChain` success; p50 / p95 / p99 / max.
- Time under the activation lock per block (instrumented: `AcquireBlockIngressActivationLock` hold time), p95 / max.
- RPC latency during validation: `getblockcount`, `getrawmempool`, `getblocktemplate` p50 / p99 / max, and count of responses slower than 1 s.
- Template staleness: how often two consecutive `getblocktemplate` calls 2 s apart return the same tip while PEER's tip has advanced.
- Mempool admission: accepted / rejected / time-to-decision p99 for valid and invalid shielded txs.
- CPU utilization of the daemon (1 s samples, mean and p95) and peak RSS; verifier pool queue depth and `rejected_budget` count (new design only).
- Catch-up: blocks per second while syncing W2; fork: time to converge in W3.
- Failure counters from the log: SAFE MODE, INVARIANT VIOLATION, corrupt, REORG ABORT.

## 4. Harness

Python, one script, `tests/integration/load/shielded_load.py`, reusing the existing regtest helpers (node start/stop, mining, shielded RPCs); it writes one JSON file per scenario with the raw samples and a Markdown summary. It runs the same code path for old and new: the only switch is which transaction builder it calls (v1 Auth transfer vs v7 bundle). The harness itself is not consensus code and can be built and validated now against the old design.

Section 4.1 (exact node flags, RPC names, helper functions) is filled from the repository's integration helpers; see the baseline run notes appended below.

## 5. Pass criteria (responsiveness, per host class)

Proposed, for the owner to confirm or revise:

| bound | fleet class | small-node class |
|---|---|---|
| block latency p99 at NUT under W1 | ≤ 2 s | ≤ 10 s |
| activation-lock hold p95 per block | ≤ 500 ms | ≤ 3 s |
| `getblocktemplate` p99 during W4 | ≤ 1 s | ≤ 5 s |
| `getblockcount` p99 during W4 | ≤ 100 ms | ≤ 500 ms |
| template staleness events in W4 | 0 | ≤ 5 |
| invalid-proof rejection p99 (W5) | ≤ 1 s | ≤ 5 s |
| W5 must not raise W1 block latency p99 by more than | 25% | 50% |
| catch-up W2 | ≥ 5 blocks/s | ≥ 1 block/s |
| daemon CPU mean under W1 | ≤ 50% of cores | ≤ 80% of cores |
| failure counters | 0 | 0 |

The old design is expected to miss several of these (it is why v2 exists); its numbers are the comparison baseline, not a pass/fail.

## 6. Sequence

1. Harness against the OLD design on the M4 (dry run, harness validation, first baseline). Executed now; results appended below.
2. Old design on the small-node class (GitHub runner or equivalent VM) and, when the owner designates one, on the fleet class.
3. Independent review of the statement/profile → consensus wiring (Tasks 1–4, 6) → new design on the same three hosts with the same harness.
4. Owner decides thresholds and the hardware contract on the paired results.

## 7. Evidence format

Per run: host description (`lscpu`, cores, memory), daemon commit, harness commit, the raw JSON, the summary table, the daemon log. Committed under `docs/benchmarks/load/<date>-<design>-<host>/`.
