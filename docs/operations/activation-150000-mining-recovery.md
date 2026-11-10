# Activation 150000 mining recovery

The released embedded solo miner requested legacy `getblocktemplate` work. At the
Orchard boundary, that protocol cannot carry the typed body and proof. The daemon
also acquired its mining owner before rejecting legacy callers, causing each
retry to independently replay the entire historical prefix.

The fix routes incompatible requests before preparation, uses immutable
`mining.getjob` / `mining.submit` work in the embedded CPU and GPU miners, and
shares only completed immutable boundary proof material. Request lifetimes and
unfinished replay remain thread confined. A per-service try-lock prevents
concurrent boundary replays. Cached material is bound to the storage owners,
parent height/hash/work, full profile, and historical storage revision. Staged
historical writes invalidate the cache again at commit. All selected live/durable
coin, forest, shielded and canonical-history comparisons remain required.
Successful HTTP submissions may return either null or an empty object; both
represent the same accepted result. A submission generation fences delayed job
responses, and CPU/GPU winners are checked against current work before taking
the submission slot. No consensus rule or activation height changes.

The live canary also exposed cold empty-wallet recovery replaying the same
historical prefix under the selected lock. It now uses the shared detached
preparation before taking the snapshot and selected locks. Legacy shielded-tree
roots are memoized under their existing mutex, copied with the frontier and
invalidated on append, truncate, and restore. The original Poseidon calculation
and serialized frontier are unchanged. An independent uncached oracle checks
all mutations and ownership transfers; exact positional K5 allowances cover
only those cache insertions, without replacing the consensus baseline.

At the activation parent, WalletService startup defers canonical recovery to the
existing runtime worker and suppresses legacy scanning until that recovery is
checked. The historical index audit is reused only at the same database input
revision; selected coins, durable coins, forest, shielded state and markers are
still compared on every request. Hash-addressed additions outside a completed
prefix do not invalidate it. Existing-key overwrites, deletes, canonical index
changes and reopen do. Historical writes are serialized with their commit-time
existence comparison, including previously staged inserts that become overwrites.
The GUI template inspector uses immutable jobs and does not poll hidden templates.
Stored activation plans extending the selected boundary also join that same
single-flight proof. A busy preparation defers the plan; it never starts another
genesis replay. Multi-block extensions reuse only the frozen historical prefix,
then independently validate their exact Orchard branch within the original total
byte limit. Replacement, historical and snapshot plans retain their own proof
owners. Refusal diagnostics distinguish unavailable preparation from a failed
coin, forest, shielded or historical comparison.

The selected-prefix builder audits the canonical height index during detached
preparation, then binds that audit to the unchanged storage owner, selected tip
and historical revision. Forward catch-up can consume this audit without reading
the entire historical prefix again under the selected lock. Every use still
checks live and durable state, and every incoming Orchard block remains rebound
and validated. A replacement that disconnects or reconnects historical blocks
retains the full historical header and body comparisons.

## Storage preflight

Check `getnetworkinfo.storage_upgrade_refused` before waiting for activation
preparation. Orchard requires the separated ten-column-family storage layout.
A legacy nine-column-family database refuses boundary preparation immediately;
repeated genesis replay cannot repair its layout.

The storage upgrader deliberately refuses source SST files with hardlinks outside
its controlled migration pair. Old backup clones can cause this refusal. Retiring
their links changes backup retention and needs the owner's approval and a guarded
maintenance procedure; do not weaken the upgrader's alias checks. Preserve wallet
files, unique backup data, live chain data and the H149999 capture. The upgrader's
own `chaindb.legacy-v1` rollback database and its paired SST links are expected and
must remain intact.

## Snapshot preservation

EU1 and NA independently captured H149999, block
`000000008255d0bcf97042e30ef41f521b2647c0f3790f6cd647ef75c84a1d48`.
The v5 payload is 68,905,572 bytes with SHA-256
`4f05b159a911a97f7f1ddf5cb2e5c31880e445063e1e569aa5f771aeaac20bea`.
Original `snapshots/pending.dat` and `pending.json` must remain intact. Independent
preservation copies are under
`/var/lib/dinero/snapshot-preservation/h149999-4f05b159/` on both hosts.
The daily publisher remains held. Compact v6 staging uses its separate namespace;
publication still requires burial and genuine phone qualification.

## Qualification

Run the exact OrchardParentService, OrchardParentReplay, OrchardParentDrain,
OrchardMiningRpc, OrchardMiningTemplate and OrchardDetachedMining suites. The
embedded integration suite requires `DINERO_BUILD_MINER=ON` and the real Orchard
backend. It uses authenticated loopback HTTP, the actual embedded miner and daemon
handlers, a private genuine-PoW chain, mixed shielded bodies, boundary and successor
blocks, and independent inspection of stored commitments and PoW. Metal is also
exercised when built on supported Apple hardware. The historical-start case tests
protocol continuity from the block before activation through its successors.
Also run CommitmentTree and the OrchardEmptyParentAccount/Queue suites for the
cold-wallet recovery path. Headless fleet builds use `ENABLE_GPU_MINING=OFF`
and must pass the destination host's dynamic-loader check before installation.

For rollout, retain the old binaries, stop cleanly, preserve existing datadirs,
verify the candidate binary hashes, start one canary, and require real job creation,
normal shutdown/restart, agreement with independent peers, and unchanged H149999
payload hashes before expanding. Source-build tests alone do not establish live
mainnet recovery. Do not reset wallet or chain data or bypass validation.

Initial qualification on physical Apple Silicon: 36 focused core regressions
passed; all three real-HTTP embedded mining cases passed three consecutive runs
(two CPU workers and the Metal backend). The historical-start case also delays
a real job reply across submission. Native miner parsing and Metal self-tests
passed. Live rollout and Linux artifact qualification are recorded separately
in the operator evidence, not inferred from these local results.
