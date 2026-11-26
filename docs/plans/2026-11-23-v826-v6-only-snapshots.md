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
not part of the Wallet page total. After a first Orchard deposit on a mainnet
desktop wallet the Wallet page showed "Private: 0.00000000". Shielding must
not look like money disappearing: show the Orchard balance there, include it
in the total, and keep any retired-pool amount on its own named line.
Done in 18f548b69 (Orchard line counted in the total; the retired pool only
when it holds something).

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

### Windows desktop findings (a Windows 11 laptop, v8.2.5, 2026-11-23/24)
- **A wallet created on that node never shows its money.** A wallet was
  created there on v8.2.5 and mined to. At the tip, with local mining off,
  `sync_meta.last_scanned_height` stays 0 and `wallet.snapshot` lists no funded
  address, while `getaddressbalance` shows the mined coins at its first address
  (`ismine` true).
  - First explanation, **refuted**: that the wallet's cursor could not advance
    because v8.2.5 discards a full account-replay build whenever a block
    arrives while a reader holds it. The chain then stood still for 96 minutes
    with the wallet unlocked and the cursor stayed at 0.
  - Cause found (2026-11-24): that datadir was started from the H52,287
    snapshot (since fully validated and backfilled), so its wallet index is
    schema v2 and can never be promoted to v3 (the v3 startup gate refuses a
    former-snapshot datadir); a wallet created natively on v8.2.5 is not served
    through the legacy index of such a node. It was never servable there.
  - What this branch does: such a node now refuses to create the wallet, with
    the way out, instead of creating one that stays at zero (1607d3106,
    8e99e17d4, a9eda7425). It does **not** help a wallet that already exists
    there: its recovery words restore it on a node synced from the first block
    or in the phone app. Promoting former-snapshot datadirs to v3 is not done.
- **Startup took 38-42 minutes to open RPC** (one busy thread; about 275 GB of
  reads on a 57 GB full datadir); the same step takes 4-5 minutes on a Mac.
  Not investigated.
- **The daemon log looks empty on Windows but is not.** Explorer and `dir`
  show `%APPDATA%\Dinero\embedded-daemon.log` as 0 KB while the node has it
  open; read with shared access it was 30 MB. (An earlier version of this note
  said there was no log. That was wrong.)
- **`wallet.getsyncstate` ignores its wallet-name argument** and reports the
  active wallet (`rpc_context_wallet_getsyncstate` discards `params`).
- **Orchard tab shows "Unknown"** when the node refuses a balance read while
  the wallet catches up (seen on a Mac desktop wallet). Show the last known
  balance with its block height instead. Done in 07ec77248; a refused read
  alone changes nothing on screen since 7e487e902.
