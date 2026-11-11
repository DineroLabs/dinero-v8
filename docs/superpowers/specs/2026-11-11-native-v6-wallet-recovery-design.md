# Native v6 wallet recovery (post-150000 transparent recovery)

**Status:** investigation complete (2026-11-11); design draft. Release-blocking: the compact-v6 phone
qualification cannot finish until this ships and the owner's end-to-end bar passes (full-utreexo-mode
recovery restores the FULL balance and the wallet actually switches bridge → local on both phones).
**Owner:** Claude (Core side). iOS/DPI lane owns the app/handoff side; chainstate lane adds the
refusal + typed recovery path to its graph/reorg test requirements.

## 1. The facts (measured)

- `ChainstateService::RecoverTransparentSnapshotWallet` (chainstate_service.cpp:15119) refuses at
  :15150 when the selected tip is Orchard-active. **The refusal is correct**: it is not the real
  blocker, it fail-closes over one.
- **The real blocker: a compact v6 phone has no owned-coin set at its base.** V6 containers carry
  headers/roots/catalog only — zero coin records, no forest (orchard-snapshot-v6.md:92,:112); the
  wallet-source reader accepts only V3/V4/V5 (snapshot_wallet_source.cpp:38); the catalog's
  LegacyCoins tree has outpoint/height/coinbase but no value or script. Nothing on the phone can
  enumerate owned coins at 150074.
- Post-150000 bodies are typed (`OrchardBlockCandidate`, no implicit legacy conversion): the legacy
  Merkle check, `SelectedWalletHistory` (legacy `Block`) and the rescan writer cannot carry them.
- The phone reaches recovery via RPC `wallet.recovertransparentfromsnapshot` through
  `nodecore_rpc_call`; both apps hard-code the success scope string and **retry the refusal forever**
  (it is returned `retryable:true` — a permanent capability refusal misclassified as transient).
- The bridge→local switch is app-owned: promotion requires local `wallet.getbalance` to equal the
  bridge ledger (30s freshness; exact immature-coin difference tolerated) plus chain gates. No Core
  "recovery complete" signal is consulted.
- **Existing machinery that already solves pieces of this**: `OrchardBoundaryReplay` +
  `PhoneHistoryTarget`/`OrchardPhoneHistoryDownload` (verified typed bodies activation→tip on exactly
  a compact-v6 base); `RuntimeAccountReplay::BuildWithActivationPrefix`; the sep2 coverage pipeline
  (`captureRuntimeWalletOrigin` → `RuntimeWalletCoverageProjection::Build` → reconcile writers), today
  gated to the "v5-imported-at-activation-parent" composition; `ReadPostBaseFunding` already extracts
  transparent coins from typed replay bodies.
- **The preserved H149999 V5 payload (68,905,572 B, sha256 4f05b159…, EU1+NA
  `/var/lib/dinero/snapshot-preservation/h149999-4f05b159/`) is a full container at the activation
  parent** — captured for exactly this class of need.

## 2. Chosen design: A — activation-parent V5 base + verified typed tail

Recover the transparent wallet in two authenticated halves that meet at the activation boundary:

1. **Base set at H149999** from the preserved V5 container, read by the EXISTING
  `ReadSnapshotWalletSourceFile` path (V5 accepted today; full live-leaf coverage bound to the
  H149999 header's utreexo root — same trust model as every pre-Orchard recovery). The container is
  served to phones over the existing private snapshot channel as a RECOVERY SOURCE, not installed as
  chain state (a read-only "wallet source" mode of the reader; `ReadOrchardSnapshotFile`'s
  profile-mismatch refusal stays intact because this path never touches chain state).
2. **Typed tail 150000 → tip** from the machinery the phone already runs:
  `OrchardBoundaryReplay`-verified phone-history bodies plus the runtime outbox, consumed through the
  sep2 coverage pipeline. Transparent effects extracted the way `ReadPostBaseFunding` already does;
  the wallet writer receives them through the reconcile path (which understands post-activation
  state), NOT through legacy `SelectedWalletHistory`.
3. **Composition gating**: extend the `imported_parent`-style gate to accept the late-v6 composition
  — chain base = compact v6 at/after activation, wallet source = authenticated V5 at exactly the
  activation parent whose header chains into the selected ancestry. Every existing check stays:
  sole lease, session pin, watch-script capacity, source-unchanged re-check under both locks,
  fail-closed on any mismatch.

Why not the others (recorded, not silent):
- **A′ (full-profile v6 at 150074)**: no full v6 is published, and it needs new reader surface for a
  "wallet data only" full container; A uses an artifact that already exists with a reader that
  already accepts it.
- **B (bridge candidates + WalletInputProof)**: completeness is not provable — omissions silently
  understate balance, and feeding bridge-derived candidates into a bridge-equality promotion gate is
  circular. Permitted only as a display aid; never called recovery; never a promotion input.
- **C (full owned replay, genesis→tip on the phone)**: sound and complete but ~150k bodies over P2P
  on a phone; kept as the documented fallback if A's boundary meet reveals unsound seams.

## 3. Interface and app coordination (iOS/DPI lane)

- New success scope string (the apps hard-code the old one): e.g.
  `transparent-current-coins-and-post-snapshot-history-v6`. Coordinated app change required.
- **Capability refusals become non-retryable**: the Orchard refusal (and the new path's permanent
  refusals) must return `retryable:false` so phones stop the 15-second forever-retry. Transient
  refusals (locks, sessions, downloads in progress) stay retryable.
- The bridge→local switch stays app-owned and gated on balance equality with the bridge ledger —
  that equality IS the owner's end-to-end test; Core's job is to make the local balance complete.

## 4. Test plan (nothing ships on assertion)

- **Harness (regtest, both Orchard configs where applicable):** a full end-to-end fixture — fund
  watch scripts pre-activation, cross a regtest activation boundary, produce a V5 at the activation
  parent and a compact v6 base beyond it, run the new recovery, assert the recovered balance equals
  the node's own `getbalance` EXACTLY (full restoration), including: coins created before
  maturity-leaf activation (creation-body path), coins spent in the typed tail, coins created in the
  typed tail, and an immature coinbase.
- **Refusal tests:** v6-without-V5-source refuses (fail-closed, non-retryable); tampered V5 (bad
  root binding) refuses; source-changed-during-recovery refuses; the OLD pre-Orchard path still
  works byte-identically (its 15 SnapshotWalletRecovery cases must stay green).
- **Gates:** K5 re-anchor per frozen-source edit; K6 if tx_validation.h is touched (not expected);
  P1 (no new ambient reads — the new code takes params explicitly); O1 untouched sets stay clean;
  the RPC transcript reference updated deliberately if `phase_snapshot` output changes.
- **Owner's bar (unchanged, final):** on BOTH physical phones — native import, recovery restores the
  full balance in full-utreexo mode, tip/accumulator agreement, clean shutdown, cold restart, and
  the app's bridge-equality gate flips the wallet bridge → local. Release blocked until then.

## 5. Stages

- **A: this doc** + the retryable-misclassification fix (small, immediate value: phones stop
  hot-looping on a permanent refusal) + refusal tests for the current behavior.
- **B: the V5 wallet-source mode** — read the preserved container as a recovery source against a
  non-tip header (H149999) that chains into the selected ancestry; base-set recovery lands in the
  wallet exactly as today's path does at a pre-Orchard tip.
- **C: the typed tail** — late-v6 composition gate + coverage-pipeline consumption of the verified
  phone-history bodies + outbox; transparent effects applied through the reconcile writer.
- **D: end-to-end harness test** (the §4 fixture) + serve the preserved V5 on the private phone
  channel (Codex-coordinated: EU1 serving config only, no publisher changes).
- **E: phone qualification** — iOS/DPI lane app updates (scope string, non-retryable handling),
  owner's physical runs, release decision.

## 6. Collision ledger

- chainstate lane (#903, graph/reorg): no shared files in stages A-C except chainstate_service.cpp
  — coordinate merge order with Codex's split (their split predates this; rebase order negotiated at
  PR time). The refusal/typed-path test requirements they're adding are complementary.
- iOS/DPI lane: owns everything in the apps; Core changes here only alter the RPC's outputs in the
  coordinated ways above.
- Publisher constraints unchanged: nothing public, no duplicate publishers; serving the preserved V5
  on the existing private qualification channel is a Codex-coordinated EU1 config change.
