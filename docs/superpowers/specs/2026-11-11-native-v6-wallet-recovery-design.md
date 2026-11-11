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
- **C: the typed tail** — late-v6 composition gate + a narrow tail applier over the verified
  phone-history replay (activation..base typed bodies + base+1..tip outbox), applied through the
  snapshot-recovery rescan writer. See §5a for the chosen plan and why the coverage-pipeline
  route was rejected.
- **D: end-to-end harness test** (the §4 fixture) + serve the preserved V5 on the private phone
  channel (Codex-coordinated: EU1 serving config only, no publisher changes).
- **E: phone qualification** — iOS/DPI lane app updates (scope string, non-retryable handling),
  owner's physical runs, release decision.

## 5a. Stage C plan (chosen: narrow tail applier)

Two candidate routes were studied in-code; the **narrow tail applier** was chosen.

**Rejected: reuse the sep2 imported-coverage pipeline.** That pipeline's origin contract is
load-bearing for a different composition (v5-imported-at-activation-parent) and fails the late-v6
phone on at least seven structural preconditions: the first-event shape (sequence-1 Connect at
`height == activation_height`, chainstate_service.cpp:7572; late-v6's first outbox event is at
base+1), the genesis→activation full-body replay origin (the exact cost full replay was rejected
for), the `imported_parent` cohort binding (absent on late-v6, gates all four imported writers via
`CaptureImportedOrchardDeliveryUnderLock`), `RuntimeWalletCoverageProjection::Build` consuming
outbox events only (no notion of prefix steps), `ReadPostBaseFunding`'s activation-floor throw,
and — decisively — the reconcile writer cannot express a recovered tail **send**: it inserts
receive rows only (runtime_index_delivery.cpp:1021) and hard-fails "Coverage originated history
unavailable" (:971) for observed spends of owned coins with no pre-existing history row, which is
exactly the bridge-spent-during-the-window case. Fixing all of that requires writing the tail
extractor anyway plus five receipt-coupled surfaces, and reroutes `wallet.getbalance` through the
coverage capsule. Kept from that pipeline: the fence idiom (recheck under
`snapshot_load_mutex_` + `activation_mutex_`, replay head == captured tip).

**Chosen: compose the two authenticated halves the design already names.**
Base set = stage B's V5 container (root-bound to the H149999 header). Tail = the composed
`RuntimeAccountReplay` (`getRuntimeAccountReplayForWallet`): activation..base typed bodies verified
by `OrchardBoundaryReplay` against the burial-depth-authenticated activation proof and the phone's
own selected header chain, plus base+1..tip from the phone's canonical outbox, revalidated
page-by-page. Transparent effects are extracted per block and applied through the same
`RescanBlockchainImpl` writer the pre-Orchard snapshot recovery uses (wallet_manager.cpp:7629 loop
— it needs only txid/vout/value/script/prevouts/coinbase/header-timestamp, all present in a typed
delta), in the same single BEGIN IMMEDIATE transaction, index-commits-first, with
recovered-blocks stamps extended over the delta heights.

**Steps (smallest first, one test each):**
1. Typed-tail extractor `ReadRecoveryTailEffects(const RuntimeAccountReplay&)` beside
   `ReadPostBaseFunding` (chainstate_service.cpp:21676 pattern: Orchard/Historical tx shapes,
   caps, CT refusal), walking prefix steps then events. Unit test over a synthetic replay:
   base-coin-spent-in-tail, created-and-spent netting, immature coinbase, refusals.
2. `SelectedWalletHistory` typed-tail member + rescan branch iterating deltas where the loop reads
   `block.vtx`; stamp extension. Test: snapshot+deltas recovery case; neuter check: without the
   branch, tail coins are absent.
3. Compose in `RecoverTransparentSnapshotWallet`: real late-v6 arm captures the replay, requires
   head == tip under the final locks, extracts, applies with `recovery_base+1`, returns target;
   harness-only partial mode stays behind its flag; V5 source bytes digest-rechecked under the
   final locks. Test: activation-crossing fixture, exact-balance equality; tip-advance mid-recovery
   → retryable refusal, succeeds on retry.
4. Typed refusal signal (small result struct) replacing the RPC string match; new scope
   `transparent-current-coins-and-post-snapshot-history-v6`; permanent (no V5 source, tampered V5)
   non-retryable, transient (download pending) retryable with progress.
5. Refusal matrix per §4; the 15 pre-Orchard recovery cases stay byte-identical.

**Open questions (owner/Codex):** who arms the phone-history download for recovery (reuse
`recoverOrchardAccount0`'s request path vs. the recovery RPC arming
`RequestRetainedPhoneHistoryData` itself); extractor event cap vs. `ReadPostBaseFunding`'s 4096 on
a phone weeks past import; who writes `wallet_recovery_v5_source` on-device and whether the
container sha256 is pinned client-side; whether a pre-existing ordinary-delivery receipt tolerates
the recovery rewrite (test, or clear it inside the recovery transaction); #903 rebase order for
the chainstate_service.cpp hunks.

## 6. Collision ledger

- chainstate lane (#903, graph/reorg): no shared files in stages A-C except chainstate_service.cpp
  — coordinate merge order with Codex's split (their split predates this; rebase order negotiated at
  PR time). The refusal/typed-path test requirements they're adding are complementary.
- iOS/DPI lane: owns everything in the apps; Core changes here only alter the RPC's outputs in the
  coordinated ways above.
- Publisher constraints unchanged: nothing public, no duplicate publishers; serving the preserved V5
  on the existing private qualification channel is a Codex-coordinated EU1 config change.
