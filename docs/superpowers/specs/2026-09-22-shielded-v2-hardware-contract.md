# Shielded v2: hardware and workload contract (decision document)

Status: proposal for the owner's decision, 2026-09-22. Nothing is asserted as a gate until a class is chosen.
The spec (§2) names "fleet 8-core x86" for the verify target and "the CI runner class" for the §6 gate;
these are two different machines and the documents currently do not say which one a pass is measured on.

## What has been measured, per host

| host | cores | 2-in-2-out warm entry point (serial / with budget) | 50 proofs, best budget | old design, one 8-proof block cold |
|---|---|---|---|---|
| M4 Max (macOS) | 12 | 28.4 ms / 18.1 ms | 223 ms (8 workers) | 7.8 s* |
| GitHub ubuntu-24.04 (EPYC 7763 VM) | 2 (+HT) | 67–71 ms / 54–56 ms | 1,567 ms (4 workers) | 17.3 s* |
| fleet-class idle 8-core x86 | 8 | not measured | not measured | not measured |

*from observed inter-block gaps; the spawn-timed rerun supersedes these figures when it lands.

## Options

A. **Fleet class is the contract** (an idle 8-core x86 of the servers' type, never a live mainnet node).
   Matches the spec's §2 wording. Requires designating a host; nothing measured yet. Thresholds 20 ms /
   400 ms stay as written until measured there.
B. **CI runner class is the contract** (the 2-core VM). Matches §6's "CI enforces". The proof-stage
   thresholds are not met there (≈3.4× and ≈3.9× over); adopting B means either revising the thresholds
   on operating evidence (owner has said this is possible) or accepting failure as the current state.
C. **Two-class contract (recommended):** fleet class carries the proof-stage thresholds (20 ms / 400 ms, or
   revised); the small-node class carries only responsiveness bounds from the load-test plan §5 (block
   latency, template/RPC p99, lock hold, catch-up rate), with proof-stage numbers reported but not
   asserted. Both always reported; the mobile prove gate stays with the prover kit.

## Recommendation

C, with two concrete decisions from the owner: (1) which physical machine is the fleet-class reference
(and who provisions it; it must not be a live node); (2) whether the small-node responsiveness bounds
in the plan's §5 table are the ones to enforce, or replaced by values derived from the old-design
baselines (§8 of the plan) once the spawn-timed numbers are in. Until (1) exists, every x86 statement is
"unmet on the runner class, unmeasured on the fleet class".

## Workload contract (already fixed in the plan)

Legal block budget (8 Auth proofs / 1,000,000 shielded bytes), fresh randomness, mempool-then-block vs
cold traffic kept apart, pre-generated transactions so proving never throttles the node under test,
overload labelled separately, block contents read back, medians of ≥ 5 for proof-stage rows.
