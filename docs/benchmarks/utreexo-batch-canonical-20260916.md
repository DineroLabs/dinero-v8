# Batch Utreexo verification: canonical coin lookup

## Failure and cause

PR #755, head `4a11b3c6b36bacb61050d2acd1f753ba1169b23f`, failed
`ShieldedAuthRelayLifecycle` in Actions run `35124273501`. After invalidating
and reconsidering the unshield block, the daemon had committed the output and
restored its Utreexo position. `getutxoproofs_batch` generated its proof, while
`verifyutxoproofs_batch` returned `utxo-not-found`.

The verifier called `ChainstateService::getUTXOIndex()`. Despite an outdated
comment describing this as consensus authority, that accessor returns the
wallet-owned SQLite index. The reconnect log shows the wallet worker queued
behind canonical block application. The same error is deterministic on a node
whose wallet does not own the output, with no race required.

## Change and limits

Verification reads value, script, creation height and coinbase status from
`ChainDB::getCoin`, the canonical source already used by `getutxoproof`.
Missing coins, database errors and malformed stored metadata fail closed.
`HashUTXOForCreationHeight`, forest snapshot locking, proof verification,
consensus validation and accumulator mutation are unchanged.

This is a real proof-serving RPC fix, not a sleep/retry or relaxed assertion in
the failing harness. It does not activate compact transaction encoding. Proof
requests spanning a concurrent chain transition can still require a refreshed
proof; this patch does not introduce an atomic multi-RPC snapshot contract.

## Test-first qualification

`UtreexoBatchProofCanonicalCoins` starts two isolated regtest nodes, mines to
one wallet, and confirms the selected mature coin exists canonically but is
absent from the other wallet. Before the fix, proof generation succeeds and
verification fails with the same `utxo-not-found` error as CI. After the fix:

- Both nodes verify the coin, including after non-owning peer restart.
- Corrupting a sibling is rejected by the unchanged proof verifier.
- A nonexistent outpoint is rejected.
- A signed child spends the exact coin; its old proof is rejected on both nodes.
- Disconnect restores the coin and a verifiable proof; reconnect restores
  spentness on both nodes.

Local macOS arm64 regression passed in 19.76 seconds. The original full
`ShieldedAuthRelayLifecycle` also passed, including real shield/transfer/unshield,
restarts, disconnect/reconnect commitments, and mining its signed transparent
child. The new test is registered and explicitly selected in the serial CI
lane; no baseline exemption is added. Full Linux qualification remains pending.

Raw red/green logs, the preserved old daemon, and CI-selection evidence are in
this worktree's ignored `build-review/evidence/` directory.

## Restart relay readiness follow-up — 2026-09-18

Post-merge [Tests run 35371469326](https://github.com/DineroLabs/dinero-v8/actions/runs/35371469326)
at `512812f78b6b1346e2dd416af0068ddda6f4d36c` passed both canonical-coin
verification checks, then failed waiting for the signed child to reach the
restarted peer's mempool. The owner log places its one-shot INV announcement
after losing the old connection and before the replacement handshake.
Matching persisted tips and working proof RPCs do not establish relay readiness.

The fixture now requires `getpeerinfo` to report a connected, version-negotiated
peer at both ends before submitting the child once. Both nodes' only configured
connection target is each other. The bounded readiness loop reports both peer
inventories on failure. Proof verification, corrupt-sibling and missing-outpoint
rejection, the signed spend, remote mining, rollback/reconnection and the
300-second CTest timeout are unchanged. No rebroadcast or acceptance retry was
added. This changes no daemon or consensus behavior and does not establish an
offline-transaction rebroadcast policy.

Local qualification uses two real regtest daemons. A separate test adapter
disables the restarted peer's networking, verifies equal tips and zero actual
connections, then releases networking on the first readiness or remote-mempool
query. It does not fabricate RPC results. The original fixture broadcasts first
and fails to relay; the corrected fixture waits for a real handshake, then passes
every original assertion. Removing only the readiness call reproduces the same
relay failure as a behavioral negative control. The normal fixture also passes
without the adapter.

CI now collects both nodes' retained console and daemon logs under the existing
CTest artifact, capped at 20 MiB per file. Visible filenames keep them eligible
for the artifact uploader; cookies and wallets are excluded. The actual YAML
step was exercised with six synthetic log paths, an over-cap log and synthetic
cookie/wallet files. Workflow-parser self-tests pass, and the canonical-coin test
remains explicitly selected by the serial lane. Fresh Linux CI is still required.
