# v8.2.6 — v6-only bootstrap snapshots (plan, 2026-11-23)

Owner request: remove the obsolete pre-Orchard snapshots and keep only v6
(Orchard) snapshots. Exception (owner decision): the H149999 v5 artifact stays,
**only** as the phone wallet-recovery source, never as a bootstrap snapshot.

## What was verified before planning

- **Startup is safe.** When a node's persisted AssumeUTXO lifecycle has no
  matching bundled snapshot but the node is bootstrapped at or above the base
  (the normal case), `chainstate_service.cpp:5185-5296` logs "snapshot file is
  moot" and continues. Only a node whose import never completed fails, and that
  path is corruption, not rotation. Removing bundled files therefore cannot stop
  an already-bootstrapped node.
- **The compiled anchors are load-time only.** `AssumeUTXORegistry` holds five
  v4 anchors (52287, 65300, 73035, 84131, 99677). It is consulted only when a
  snapshot is loaded (`chainstate_service.cpp:863` manifest path, `:17197` .dat
  path), never at resume or background-validation completion. v5/v6 snapshots
  never use it.
- **Deleting anchors alone would weaken loading.** Without an entry, an old v4
  file would skip the extra check instead of being refused. So the rule must be
  explicit.

## Rule

On mainnet, the node refuses to bootstrap from any snapshot whose base is below
the Orchard activation height (150000). Regtest/testnet keep their fixtures.
The five v4 anchors then become dead code and are removed with their tests.

## Inventory (layer × action)

| Layer | Files | Action |
|---|---|---|
| 1. Release assets | `packaging/release-assets-v8.2.6.json` (new) | Only v6 sets + H149999 recovery set if still distributed. Historical `release-assets-v8.1.*/v8.2.3/v8.2.5.json` stay as records. |
| 2. Packaging/CI | `.github/workflows/{desktop-release,macos-desktop-candidate,windows-release}.yml`, `packaging/mac/build-installer.sh`, `packaging/windows/build-server-installer.ps1` (+ `build-installer.ps1` variables), `packaging/linux/stage-snapshot-pair.sh`, `Dockerfile`, `docker-entrypoint.sh`, `packaging/docker/{Dockerfile.full-v6,entrypoint-full-v6.sh,check_recovery_manifests.py}`, `packaging/snapshot/desktop-inputs-candidate.json`, `scripts/{dump-snapshot.sh,assumeutxo/generate_*.sh}` | Stop bundling/downloading 148375-v5, 99677-v4, 73035-v4, 65300. |
| 3a. Qt | `qt/src/main.cpp:128-130` | Remove the three pre-v6 fallback names. |
| 3b. Node | `src/consensus/assume_utxo.cpp` | Add the mainnet below-150000 bootstrap refusal; then remove the five v4 anchors. |
| Tests | 27 files incl. `test_assumeutxo_anchor_{73035,84131,99677}.cpp`, `test_snapshot_format_policy.cpp`, CI desktop-input tests | New failing test for the refusal first; retire anchor tests; keep regtest fixtures. |
| Site | dinerolabs.org `install.sh` fallback logic | Confirm it never selects a pre-v6 asset. |

## Decision (owner, 2026-11-23): B — newest v6 + one previous

How many v6 fallbacks to bundle. Each v6 snapshot is ~136-143 MB:

- **A. Newest v6 only** (H163965): smallest installers (~410 MB less than v8.2.5).
- **B. Newest + one previous** (H163965 + H155650): covers an install interrupted
  mid-import on the immediately previous release.
- **C. Keep all four v6** as in v8.2.5: no size change from v6.

## Sequence

1. Failing test for the mainnet below-150000 refusal; implement; neuter-check.
2. Remove the five v4 anchors and their dedicated tests.
3. Qt fallbacks, then packaging/CI per the chosen fallback option.
4. Confirm H149999 still reaches the phone wallet-recovery path (not LoadSnapshot).
5. Full suite + Orchard root builds + desktop/Windows/Mac CI before merge.

Related publisher hardening (separate PRs): #974 (format-downgrade guard on
dinero-main), #975 (phone prepare re-arm, on codex/compact-v6-phone-publication).

## Progress

- Step 1 done (2026-11-23): `RejectPreOrchardBootstrap` in `EvaluateSnapshotFormat`
  (`require_orchard_base`), enabled only in the mainnet `LoadSnapshot` path.
  `OrchardOnlyBootstrapRefusesEveryPreOrchardBase` failed to compile before the
  change (red) and passes after; format policy 14/14.
- Operational note: EU1's publisher gate loads candidates with the installed
  dinerod. A v8.2.6 dinerod refuses the still-pending H149999 v5 boundary
  capture, consistent with keeping H149999 only as the phones' bundled
  wallet-recovery source.

## Related NodeCore bugs found on the Android phone (2026-11-23)

1. **Block apply rescans the whole header gap per block.** After a fast sync
   from H150074 with headers at ~H164947, `din-blkapply` spent >50 CPU-minutes
   at H150334 (blocks there are coinbase-only). simpleperf (no root; evidence
   `~/src/dinero-evidence/2026-11-23-android-h153110-stall/perf-blkapply-*`):
   `QueuedOrchardBlock::ValidateAndApply` → `TryAcceptOrchardBlockFromRPC` →
   `ActivateBestChain` → `ActivateBestChainPass` spends its time in
   `HasStoredBlockBody` (fstat + header-metadata RocksDB read + hex decode per
   header) and `AddCandidate` → `HasBranchDataToConnectedBase`. Cost per block
   scales with the unapplied gap, so a node far behind crawls. Fix: bound the
   pass (stop at the first missing body / cache per-header body presence),
   with a regression test that fails on gap-proportional work.
2. **Failed start leaks the datadir lock.** After a force-stop mid-apply, one
   `nodecore_start` failed and left `dinerod.lock` plus the chaindb LOCK/LOG
   open in the app process with no node threads; every later start returned
   `NODECORE_ERROR_INIT_FAILED (-3)` ("Start stateless node" did nothing) until
   the process was killed. Fix: release the DatadirGuard and databases on every
   failed-start path; test a forced Init failure followed by a successful start.

## Release blockers and known items added 2026-11-23 (swap/core session)

### BLOCKER: the Qt Wallet page does not show Orchard funds
The Wallet page's "Shielded / private → Private:" line (`lblShieldedBalance_`,
`qt/src/mainwindow.cpp`) is fed by `wallet.shieldedbalance`, which is the old,
retired shielded pool. Orchard funds appear only on the Orchard tab and are
not part of the Wallet page total. The owner made his first Orchard deposit
(25 DIN, wallet Hotel, block 165,213) and the Wallet page shows "Private:
0.00000000". Shielding must not look like money disappearing: show the
Orchard balance there, include it in the total, and keep any retired-pool
amount on its own named line. Not started.

### Phone reorganisations (branch `claude/v826-phone-reorg`, 3b9ac9593 + a2c595541)
Android, mainnet 165,229: a 4-block reorganisation wedged the phone ("Drain
has failed 50 consecutive ticks") and then refused every start ("Retained
Orchard header unavailable"). Both fixed and exercised on a copy of the
phone's datadir with a Mac `dinerod` in the phone profile. Not run on an
Android build.

The wallet replay after such a reorganisation is **argued from source, not
observed on a device**: a kept phone replay lives only in memory, so the copy
could not reproduce it. The argument: no hand-out without a standing
checkpoint; every hand-out passes `adopt` (replay end and checkpoint on one
header chain, replay head equal to the node's outbox head); extension reads
the node's own outbox including its Disconnect events
(`ExtendTailMatchesFullBuild`); the verified checkpoint is dropped as soon as
its header is deselected (`ReorgAboveVerifiedPrefixKeepsReplayAndReplacesRequest`).

### Known, low priority: readers that cannot read a block holding an Orchard transaction
`ChainstateService::getBlockByHash` fails for such a block. Fixed for the
explorer (`claude/v826-orchard-block-lookups`). Still using it:
- Utreexo/CSN serving and proof paths in `daemon_app.cpp` (MSG_UTREEXO_BLOCK
  serving, proof request and gossip providers, the StatelessNode block
  provider). Not a sync blocker: `SelectBlockRequestInventory` makes a
  stateless node request plain MSG_BLOCK at every Orchard-active height.
- the old shielded output feed and witness RPCs, `ReadWalletRpcBlock` users,
  the DPI header anchor: not audited yet.
- `getblockfilters` now lists such heights in `unavailable_heights` but builds
  no filter for them; what that filter contains is undecided.

### Windows desktop findings (owner's laptop AHLAPTOP, v8.2.5, 2026-11-23)
- **A fresh wallet on a full node never catches up on a slow CPU (evidence, cause unconfirmed).**
  Wallet Juliett was created that morning on v8.2.5 and mined to. At the tip,
  with local mining off, `sync_meta.last_scanned_height` stays 0 and
  `wallet.snapshot` lists no funded address, while `getaddressbalance` shows
  1,100 DIN at its first address (`ismine` true) and `dinerod` keeps about 1.15
  cores busy. Expected cause: the fresh wallet's cursor advances only through
  the runtime account replay (full-archive path), and v8.2.5 discards a full
  build whenever a block arrives while a reader holds it; the blocks come from
  the network, so local mining is irrelevant. 6f06765cc, dd5521b00 and
  684ad9b43 on this branch are the fix. Confirmation still owed: pause the
  owner's Mac miner long enough for one full build on that laptop and watch
  the cursor leave 0.
- **Startup took 42 minutes to open RPC** (one busy thread, 3,152 CPU-seconds)
  on a 57 GB full datadir; the same step takes 4-5 minutes on the owner's Mac.
  Not investigated.
- **No daemon log on Windows:** `%APPDATA%\Dinero\embedded-daemon.log` is 0 KB,
  so nothing can say what the node is doing during that start.
- **`wallet.getsyncstate` ignores its wallet-name argument** and reports the
  active wallet (`rpc_context_wallet_getsyncstate` discards `params`).
- **Orchard tab shows "Unknown"** when the node refuses a balance read while
  the wallet catches up (seen on the Mac, wallet Hotel). Show the last known
  balance with its block height instead.
