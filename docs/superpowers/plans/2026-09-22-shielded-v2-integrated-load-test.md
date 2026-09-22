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


### 8.6 Corrected-harness rerun of the steady scenario (independent probes, cumulative-CPU sampling)

Evidence `docs/benchmarks/load/2026-09-22-old-m4max-v2/` (steady.json, steady.raw.json with the raw time series).
480 s, achieved 0.060 tx/s (29 builds, 0 failed), 18 blocks, 29 shielded tx confirmed by reading block contents.
Daemon CPU (Δ cumulative process time / Δ wall): mean 102% of one core, p95 108% (494 samples); the earlier instantaneous estimate (36%) under-reported it. RSS max 909 MB.

| probe | n ok | p50 | p95 | p99 | max | missed ticks | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount every 1 s | 500 | 1 | 1 | 1 | 3 | 0 | 0 |
| getrawmempool every 1 s | 482 | 1 | 300 | 2779 | 2999 | 18 | 17 |
| wallet.shieldedbalance every 1 s | 158 | 1 | 10666 | 21045 | 21285 | 342 | 54 |
| getblocktemplate every 2 s | 250 | 9 | 2023 | 2805 | 2893 | 0 | 17 |

Wallet builds: shield p50 7046 ms, transfer p50 24332 ms; block generate p50 78 ms (own mempool, cached proofs).
Reading, now with coverage stated: the chain RPC never missed a tick; the mempool RPC missed 18 of 500 ticks
and stalled up to 3.0 s (p99 2.8 s) while transfers were admitted; the wallet balance RPC missed 342 of 500
ticks because each call waited behind proving (p95 10.7 s, max 21.3 s), so "1 ms" describes only the
158 calls that got through; the template RPC (bracketed against a stable tip) stalled to 2.0 s at p95 and
2.9 s max with no stale template observed. The old design saturates one core continuously at 0.06 tx/s.


### 8.7 Hostile lane, attributed (corrected harness; evidence `docs/benchmarks/load/2026-09-22-old-m4max-hostile-v3/`)

Seeds: one output-only shield (46,481 B, 1 proof) and one 1-in-2-out transfer (167,903 B, 3 proofs), both
built last with nothing mined afterwards, captured while in the mempool, mempool cleared, and each
re-accepted by `testmempoolaccept` before mutation. The harness decodes the bundle wire layout itself
(encrypted notes are 757 bytes here, not the 611 of the spec comment: the outgoing-recovery envelope
adds 146) and records, per submission, the byte offset and the named field hit. Two lanes: **proof**
(mutations only inside `*_zkproof`, 4 of 5 submissions) and **ciphertext control** (only inside
`*_encrypted_note`, 1 of 5; no consensus check covers those bytes, acceptance is the expected outcome).

| lane | decisions | accepted | rejected | malformed / transport / protocol / 503 |
|---|---|---|---|---|
| transfer_3proofs / proof | 671 | **0** | 671 | 0 / 0 / 0 / 0 |
| shield_1proof / proof | 671 | **0** | 671 | 0 / 0 / 0 / 0 |
| transfer_3proofs / ciphertext control | 167 | 167 (expected) | 0 | 0 |
| shield_1proof / ciphertext control | 167 | 167 (expected) | 0 | 0 |

Every proof-lane rejection carried the node's reason `Shielded validation failed: proof-invalid`
(stage confirmed by the node, not inferred from time). Qualification: **passed** (no acceptance in a
proof lane). The 14 acceptances in the previous run (`2026-09-22-old-m4max-v2/`) all mapped to
encrypted-note bytes once the real 757-byte note length was known; that run also showed the wallet
re-spending the shield seed's coin because funding was mined after the seed was built, which the new
seed ordering prevents.

| decision time (ms) by lane / reason | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| transfer_3proofs/proof / proof-invalid | 671 | 11 | 931 | 946 | 1395 |
| shield_1proof/proof / proof-invalid | 671 | 5 | 522 | 830 | 848 |

Bimodal by construction: a corrupted compressed point fails proof deserialization (≈5–11 ms); a
corrupted scalar runs the verifier to its final check (≈520 ms for one output proof, ≈930 ms when
the spend proof is reached first in the 3-proof bundle; max 1.4 s). Bundle verification stops at the
first failing proof, so a 3-proof crafted transaction costs at most one full verification here, not
three; the "4.5 s per crafted 2-in-2-out" line in §8.3 was wrong and is withdrawn.

| probe during hostile (one client, back to back) | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 301 | 1 | 1 | 1 | 3 | 0 | 0 |
| getrawmempool | 301 | 326 | 823 | 924 | 1341 | 0 | 3 |
| wallet.shieldedbalance | 301 | 1 | 1 | 2 | 3 | 0 | 0 |
| getblocktemplate | 151 | 350 | 920 | 1209 | 1503 | 0 | 6 |

Daemon CPU mean 100% of one core (297 samples). While a full verification runs, the
mempool and template RPCs wait for the mempool lock (p95 ≈0.9 s); chain and wallet RPCs are unaffected.
One earlier submission produced `exception: vector` from the node on a mutated bundle (a C++ exception
surfacing as an RPC error instead of a clean reject); recorded for a robustness issue.

### 8.8 Two-node baseline: cold proofs, catch-up (W2) and reorg (W3), M4 Max

Evidence `docs/benchmarks/load/2026-09-22-old-m4max-twonode/` (two_node.json, raw time series, both node
logs). Producer node A and node under test B, both `--listen=1`, each `--connect`ing the other (the
repository's relay-test topology). B is offline while A builds and mines each block, so B has never seen
the proofs it validates on reconnect. Each A block carries the legal budget: 2 transfers + 2 shields =
8 Auth proofs (receipts read back from block contents: 8 / 8 / 8).

**W2 catch-up:** 3 blocks, 24 cold proofs. B reached the tip 23.6 s after restart; per block 7.8 s
(2 Auth spend verifications ≈ 1.4 s each + 6 output verifications ≈ 0.8 s each ≈ 7.6 s, matching the
proof-stage measurements). B ran one core at 99%. Chain, mempool and wallet RPCs stayed at 1–3 ms with
no missed ticks: block validation does not hold the RPC server. `getblocktemplate` answered 6 calls and
missed 6 ticks with a 7.9 s maximum: the template path is blocked for the whole validation of a block
(the activation lock), i.e. the #799/#803 starvation vector reproduced with legitimate cold proofs.

**W3 reorg:** B mined 2 blocks alone; A produced 3 blocks with 24 proofs; on reconnect B abandoned its
chain and converged to A's tip in 23.4 s (heights 145/146/147 at 8.0 / 15.5 / 23.3 s), CPU 97%, no
failure counters, no REORG ABORT. `getblocktemplate` answered 2 calls, missed 10 ticks, maximum 23.2 s:
blocked for the entire reorg.

| B under cold-proof load | per block | template path blocked |
|---|---|---|
| catch-up, 8 Auth proofs per block | 7.8 s | up to 7.9 s per block |
| reorg, 3 blocks × 8 proofs | 7.8 s | 23.2 s continuous |

Projection for the same blocks under v2, labelled as such (no integrated v2 path exists): each 8-proof
block is 4 bundle proofs (two 1-in-2-out transfers, two shields); at the measured 30–40 ms serial per
bundle on this host that is ≈ 0.15 s of verification per block against 7.8 s, and the plan's bounded
prewarm (Task 6) moves even that off the activation lock. To be measured, not assumed.

### 8.9 Baseline conclusion, updated

Old design, at the load it can generate: chain RPCs are never starved; wallet RPCs wait behind proving
(up to 21 s); mempool and template RPCs wait behind proof verification on admission (≈3 s per transfer,
≈0.5–0.9 s per crafted invalid proof); and, the decisive finding, the template path is blocked for the
entire validation of every peer-delivered block with cold proofs (7.8 s per legal 8-proof block on the
M4, the whole 23 s of a 3-block reorg). On the small-node class these scale by the ≈2.7× per-core factor
measured for the proof stage. These are the comparison baselines for the v2 run; they are not §5 verdicts.

### 8.10 Same harness on the small-node class (GitHub ubuntu-24.04, EPYC 7763, 2 cores / 4 vCPUs)

Workflow `shielded-v2-load-baseline`, run 35722951455 on 98e9af7d7; evidence `docs/benchmarks/load/2026-09-22-old-ubuntu-2core/`.
Harness self-tests passed on the runner.

**Steady (300 s):** achieved 0.030 tx/s (shield 16.6 s, transfer 57.6 s of proving), 9 blocks, 9 shielded tx
confirmed; daemon CPU mean 104% of one core (p95 196%: proving spills to the second core), RSS max 1.46 GB.
Chain RPC 0 missed ticks; mempool RPC p99 6.2 s, 24 missed ticks; wallet balance RPC 287 of 356 ticks missed,
max 51 s; template RPC p99 5.5 s (5 missed). The M4 pattern, ≈2.4–2.7× slower.

**Hostile (60 s inside steady):** proof lanes 0 of 126 accepted, all `proof-invalid`; ciphertext control 30 of
30 accepted (expected). Full verification of a crafted proof: ≈1.0 s (output) / ≈2.0 s (spend first); while
it runs, `getrawmempool` p95 1.2 s and `getblocktemplate` p50 0.97 s / p95 1.8 s.

**Two-node W2 (2 blocks × 8 cold Auth proofs):** B reached the tip 17.3 s after restart, i.e. **17.3 s per
cold 8-proof block**, CPU 93% of one core; chain/mempool/wallet RPCs 1–3 ms unaffected; `getblocktemplate`
blocked for the whole block validation (17.3 s max, 7 of 9 ticks missed).

**Two-node W3:** reported as not converged, and that is a harness defect, not a node result: with two blocks
per phase, A's fork side (2 blocks) was not longer than B's own 2 blocks, so no reorg was possible; B held its
tip for the full 1,800 s window at 1–2 ms RPC latency and 1% CPU. Fixed (`fork_sides`: B mines one block fewer
than A, asserted; unit-tested); the next push re-runs this on the runner.

| cold 8-proof block validation, old design | M4 Max | small-node class |
|---|---|---|
| per block | 7.8 s | 17.3 s |
| template path blocked per block | ≈7.9 s | ≈17.3 s |

At a 60-second block interval, a small node spends ≈29% of the interval validating one legal 8-proof block
with today's proofs and serves no template during it; a 3-block catch-up or reorg costs ≈52 s. The v2
projection for the same blocks (4 bundles ≈ 0.3 s serial on this host, and off the activation lock with
the bounded prewarm) remains a projection until the integrated v2 path exists.

### 8.11 Corrected small-host rerun (run 35730164447 on b78e96315) and a timing-window correction

Hostile: proof lanes **0 of 538** accepted (all `proof-invalid`), ciphertext control 134 of 134 accepted.
W3 with the fork fix converged, but convergence was already true at B's first RPC sample: B validated
A's cold blocks during its own startup, before the RPC server accepted calls, so the "0.0 s" is a
measurement-window artefact and earlier W2 "time to tip after restart" figures were likewise measured
from RPC readiness, not from process spawn. Corrected: the harness now measures from spawn, reports the
startup-to-RPC gap, the number of blocks validated before the first sample, and whether convergence
preceded the first sample. The M4 and small-host two-node numbers are re-measured with this fix below.

### 8.12 Enforced outcomes (owner review of b78e96315, commit e877b3b78)

Two review findings closed: the workflow had converted harness exit codes into success (a green run
whose saved reorg result said `qualification_failed: true`), and incomplete runs could report a pass
(zero proof decisions with thousands of transport errors). Now every scenario writes its failure
reasons (`qualification_failures`) into its result and an `OUTCOME.json`, exits 2 on any, and the
workflow runs all scenarios, then an "Enforce outcomes" step fails the job on any non-zero harness
exit or saved failure and writes `ENFORCEMENT.txt` into the artifact, which is always uploaded.
Rules, in one place in the harness: per proof lane ≥ 20 decisions and ≥ 3 decisions that reached the
verifier (> 100 ms); (transport + protocol + malformed) ≤ 5% of attempts per lane; any acceptance in a
proof lane fails; a ciphertext control lane that never accepts is flagged (layout attribution
suspect); SAFE MODE / INVARIANT VIOLATION / corrupt / REORG ABORT in any daemon log fails; steady must
build, mine and confirm at least one shielded transaction; two-node must sync and converge with cold
proofs present, and an incomplete two-node scenario is a failure. Seventeen stub-RPC tests cover the
rules. Green now means "all scenarios ran to completion with the required coverage and no failure";
it still asserts no performance threshold.
