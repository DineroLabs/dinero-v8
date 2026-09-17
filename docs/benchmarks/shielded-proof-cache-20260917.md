# Shielded proof reuse and state RPC availability

Local macOS ARM64 qualification, 2026-09-17. Linux qualification remains a
separate gate. No activation, release, deployment, or consensus-rule change.

## Reproduction

A real PoW-enforced regtest daemon and SV2 pool mine an empty recovery block
after a proxy injects failure for one compact shield's transparent-input proof.
The shield remains in the mempool. Leave the pool running with its one-second
poll and two-second same-tip refresh settings. Sample `daemon.shieldedroot`
every 100 ms for 30 seconds, with no miner submitting another block.

The unmodified daemon repeatedly verifies the same shielded output proof while
`Mempool::selectTransactionsForBlock` owns the chainstate activation mutex.
The pool polls GBT even before deciding whether to publish a new job. Its
exclusion rebuild also enters transaction selection. Native process sampling
located the work in `VerifyOutput`, circuit construction/hashing and Spartan
verification under that mutex. The root RPC's intentional `try_lock` reports
`shielded_state_busy` throughout most or all of this workload.

The first observation had 293 busy responses and zero successful reads. The
regression requires at least one consistent read every five seconds during
this fixed-tip workload; it does not require a successful read during a state
mutation. Other RPC errors fail immediately. Every successful root must equal
the final quiescent root and the accepted block's exact DNRS. The existing
transaction-exclusion, retained-mempool, stable-tip and real-PoW checks remain.

## Measured results

| Build | Successful reads | Busy responses | Longest success gap | Exclusion rebuilds |
|---|---:|---:|---:|---:|
| Before, regression | 3 | 290 | 20.688 s | 9 |
| Candidate | 288 | 1 | 0.205 s | 11 |
| Cache hits disabled (negative control) | 1 | 292 | 16.283 s | 9 |
| Candidate restored | 290 | 1 | 0.204 s | 10 |

Both negative runs fail the five-second budget. Both candidate runs pass.
The earlier exploratory run with zero successes was measurement-only, before
the regression assertion was installed. Machine-readable observations and
binary identity are in [the adjacent JSON](shielded-proof-cache-20260917.json).

## Change and validity boundary

`VerifySpend` and `VerifyOutput` each retain at most 1,024 successful proof
checks. Entries contain only SHA-256 digests in bounded FIFO storage, not the
proofs or circuit layouts. A miss runs the original verifier. A failure is
never inserted. No cache lock spans verification or acquisition of another
lock.

The digest covers a distinct proof-kind domain, all serialized proof bytes,
every public input, and all verifier switches: public-input binding,
cv-binding, spend authority, and private covenant. Covenant output root and
minimum height are included. Integer encoding is explicit. Invalid profile
combinations and unavailable Pedersen generators still fail before lookup.
The process-local cache cannot survive an executable change or restart.

Only the pure cryptographic result is reused. Bundle framing, height gates,
nullifier spentness, anchor validity, value balance, binding signature and
range proofs remain independently checked. Maturity and Utreexo paths and all
chainstate locking are unchanged. Neither a transaction ID nor an admission
decision substitutes for a proof-cache key.

## Tests

- The existing `ShieldedCvBinding` CTest now also covers all key fields/profile
  switches, duplicate insertion, bounded eviction and concurrent access. It
  remains in its existing mandatory CI lane.
- Fixed daemon-produced compact vectors verify twice, then reject modified
  public inputs, proof bytes, truncation and wrong profiles. Already verified
  bundles still reject a spent nullifier, invalid anchor and rollback below
  compact activation. Fixed Utreexo leaf/proof literals remain checked.
- Negative control: omitting the spend-authority switch from the cache key
  causes the key-isolation test to fail; restoring it passes.
- `ShieldedGeneratorFailClosed`, `ShieldedCvBinding` (8 cases), and
  `CompactRegtestFixedVectors` (3 cases) passed locally.
- `ShieldedValidation` (54 cases), `ShieldedReindexEquivalence`, and
  `MempoolTemplateMaturity` (5 cases) also passed.
- The full compact Auth relay lifecycle with timing activation at 124 passed:
  shield, transfer, unshield, locked-wallet discovery, full/CSN restart,
  disconnect/reconnect, spending the transparent unshield output, reindex,
  and activation-crossing reorg. Current proof lookup correctly rejects the
  spent output, while its saved proof still verifies against its old root.

The paired SV2 entry point is `pow_enforced_compact_pool_state_rpc_load` in
`shared_split_e2e`. Run it with the daemon built with
`DINERO_ENABLE_COMPACT_REGTEST=ON` and the pool release binary:

```sh
cargo test --locked --release -p dinero-sv2-pool --test shared_split_e2e \
  pow_enforced_compact_pool_state_rpc_load -- --ignored --exact --nocapture
```

Set `DINEROD_BIN` and `DINEROPOOL_BIN` to those binaries. The paired Linux
workflow pins the daemon commit and uploads `state-rpc-load.json` with the
recovery artifacts.

## Limits

This removes repeated work for identical already verified proofs. First-seen
proofs and cache churn still incur verification under the existing lock, so
this does not establish an availability guarantee under arbitrary load.
It does not reduce proof-generation cost or transaction fees. Production
cadence, changing difficulty, large mempools, failover and partition recovery
retain their separate qualification gates.
