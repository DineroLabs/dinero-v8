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
| W1 steady | blocks and transactions together | PEER mines one block every 10 s; each block carries the LEGAL shielded budget (`kAuthMaxBlockProofs = 8` proofs, ≤ 1,000,000 shielded bytes: e.g. two 2-in-2-out, or eight shields); the harness offers pre-generated valid shielded txs to NUT's mempool at a stated offered rate and records the achieved rate, queue growth and rejections; block contents are read back (txids, shapes, proof counts, bytes) | 10 min |
| W1-over | overload / capacity (labelled separately) | same as W1 with the offered rate raised until the mempool grows; never by changing consensus limits | 10 min |
| W2 catch-up | recovery after downtime | NUT stopped for 30 blocks of W1-style load on PEER, then restarted; time to tip | until synced + 2 min |
| W3 fork | reorg under load | PEER and a second peer produce a 6-block fork with shielded txs on both sides; NUT switches | until converged + 2 min |
| W4 service | templates and RPC during validation | W1 load plus `getblocktemplate` every 2 s and `getblockcount`/`getrawmempool`/`getshieldedbalance` every 1 s from a separate client | 10 min |
| W5 hostile | expensive invalid proofs | W1 load plus 2 invalid-proof shielded txs/s (proof bytes mutated, everything else valid) submitted over P2P and RPC | 10 min |

Fresh randomness everywhere, and two traffic classes kept apart: mempool-then-block traffic (the block's proofs were verified on admission and hit the verification cache) and cold traffic (peer-delivered blocks whose transactions NUT never saw, and catch-up), which is where full block verification cost appears. Transactions are pre-generated on a separate machine or process so that expensive proving never throttles the offered load on the node under test.

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

## 8. Baseline run notes: old design, M4 Max, 2026-09-22 (single node)

Harness `tests/integration/load/shielded_load.py`, daemon built from 4ed198529 (`build-spike`, Release),
regtest with `--consensus-shielded-epoch-reset-height=1 --consensus-shielded-spend-auth-height=2
--consensus-state-commitment-height=3` (Auth profile live from height 2, version-6 transactions).
Evidence: `docs/benchmarks/load/2026-09-22-old-m4max/` (steady+service, 480 s) and
`docs/benchmarks/load/2026-09-22-old-m4max-hostile/` (hostile, 240 s).

### 8.1 What the harness could and could not drive

- The old design's load ceiling is the wallet, not the node: one shield takes ≈7.1 s and one
  1-in-2-out Auth transfer ≈24.6 s of proving on the M4, so the generator sustained 29 shielded
  transactions in 480 s (12 shields, 17 transfers). The plan's 5 tx/s workload is unreachable for
  the old design on any host; the baseline is what the node does at the rate the old design can
  actually produce. The new design (≈0.5 s per bundle proof on the M4) can be driven far harder.
- Single-node block acceptance hides proof cost: blocks are built from the node's own mempool, whose
  proofs were verified on admission and cached (#770), so `generatetoaddress` took 70–103 ms per
  block with 1–3 shielded transactions inside. The full per-block verification cost appears only
  when a block arrives from a peer, which is the two-node variant (W2/W3), not yet built.
- No structured `validation_ms` record exists in this build's log; block cost is the generate call.

### 8.2 Steady + service results (480 s, 18 blocks, 29 shielded tx, 0 errors, 0 failure counters)

| metric (ms) | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| wallet.shield build+submit | 12 | 7,084 | 7,208 | 7,320 | 7,320 |
| wallet.transfer build+submit | 17 | 24,555 | 24,803 | 24,844 | 24,844 |
| block generate (incl. validation) | 18 | 81 | 97 | 103 | 103 |
| probe getblockcount | 76 | 1 | 1 | 1 | 2 |
| probe getrawmempool | 76 | 0 | 0 | 1 | 1 |
| probe wallet.shieldedbalance | 76 | 3,164 | 10,811 | 10,927 | 21,740 |
| probe getblocktemplate | 58 | 74 | 3,075 | 3,077 | 3,083 |

Daemon CPU mean 36% (p95 99% of one core), RSS max 911 MB, 74 probe replies slower than 1 s, no 503.

Reading:
- Cheap chain RPCs stay at 1 ms throughout: the old design does not starve the RPC server itself.
- `wallet.shieldedbalance` waits behind the wallet's proving lock: p50 3.2 s, p95 10.8 s, max 21.7 s.
  Any wallet RPC issued while the wallet proves a transfer waits up to a full transfer's proving time.
- `getblocktemplate` shows a recurring 3.07 s stall (p95/p99) with a 74 ms median. 3.07 s is the
  time to verify one transfer's three Auth proofs (≈1.4 + 0.8 + 0.8 s) on mempool admission, so the
  template builder is blocked while the mempool verifies an incoming shielded transaction. This is a
  service-path effect the v2 bundle (one ≈30 ms verification) would shrink by ≈100×, and it is
  independent of the pool's own template cost (#805).
- These are single-node, warm-cache, wallet-limited numbers; they are the comparison baseline, not
  a pass/fail against §5.

### 8.3 Hostile lane

First attempt measured the wrong path: mutated copies of an already-mined seed were rejected at
0–2 ms by the spent-input check (734,487 rejections in 240 s, ≈3,000/s; useful as the cheap-reject
throughput but not as proof-verification load). The lane now builds a fresh unmined seed, clears the
mempool, and confirms the unmutated seed is re-accepted before mutating; results below.

Final hostile run (240 s, seed = a fresh unmined 46,481-byte Auth shield transaction, re-accepted by
`testmempoolaccept` in 4 ms from the verification cache; mempool cleared via `mempool.clear`;
mutations spread over the middle 80% of the transaction so they land inside the Spartan proof):

| metric (ms) | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| invalid-proof decision, all | 1,597 | 5 | 514 | 520 | 552 |
| invalid-proof decision, full verification (> 100 ms) | 459 | 511 | 518 | 524 | 552 |
| invalid-proof decision, cheap reject (≤ 100 ms) | 1,138 | 5 | 6 | 6 | 7 |
| probe getblockcount | 240 | 1 | 1 | 1 | 1 |
| probe getrawmempool | 240 | 251 | 483 | 505 | 512 |
| probe getblocktemplate | 120 | 6 | 514 | 517 | 517 |
| probe wallet.shieldedbalance | 240 | 1 | 1 | 1 | 1 |

0 accepted, 0 busy (503), 0 failure counters. Reading:
- A corrupted commitment point fails proof deserialization in 5 ms; a corrupted scalar runs the whole
  verifier and fails at the end after ≈511 ms (one output proof). 459 of 1,597 mutations (29%) forced a
  full verification; the submitter's loop was serialised on the node, so this is one client's rate.
- While a full verification runs, `getrawmempool` (p50 251 ms, p95 483 ms) and `getblocktemplate`
  (p95 514 ms) wait for the lock the mempool holds during proof verification; chain and wallet RPCs
  are unaffected. A fee-free crafted transaction therefore holds the mempool and template path for
  ≈0.5 s per proof on the old design; a crafted 2-in-2-out (four proofs) would hold it ≈4.5 s per
  submission. The new bundle verifies (and rejects) a crafted 2-in-2-out in ≈30 ms serial on this host.
  **Both sentences are projections, not measurements** (owner review): the measured hostile seed is a
  one-output-proof transaction, bundle verification short-circuits on the first invalid proof, and no
  integrated v2 path exists yet. The rerun below adds a measured three-proof (1-in-2-out) seed and uses
  the node's reject reason, not elapsed time, as the stage signal.

### 8.4 Baseline conclusion (single node, M4 Max)

The old design does not starve the RPC server or destabilise the node at the load it can generate,
but it serialises three service paths behind proof work: wallet RPCs behind wallet proving (up to
22 s), and template + mempool RPCs behind mempool proof verification (3.1 s per valid transfer,
0.5 s per crafted invalid proof). Block acceptance from the node's own mempool is cheap because of
the proof cache; the cost of peer-delivered blocks, catch-up and forks is the two-node work that
comes next. These numbers are the comparison baseline for the v2 run on the same host and for the
small-node class; they are not a pass/fail against §5.

### 8.5 Harness limitations to fix before the paired run

- Two-node topology for W2/W3 and for peer-delivered blocks (full block verification cost).
- A structured per-block validation time in the daemon log (none exists in this build); until then
  block cost is the generate/accept call.
- The old design's generator is wallet-bound; the paired run must drive both designs at the same
  transaction rate (the old design's ceiling) AND the new design at its own ceiling, reported separately.
- The workflow variant for the small-node class (GitHub runner) is not yet written; the harness is
  self-contained Python and runs anywhere the daemon builds.
