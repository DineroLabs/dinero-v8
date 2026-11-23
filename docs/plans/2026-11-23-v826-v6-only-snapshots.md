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
