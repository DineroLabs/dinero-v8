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
