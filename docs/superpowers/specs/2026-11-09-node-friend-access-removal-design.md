# Node friend-access removal (sep2) — design

Status: rev 1, 2026-11-09. Follow-up #2 from the wallet/node separation
(`2026-10-29-wallet-node-separation-design.md` §12). Base: `dinero-main`
3d84b96fd. Branch: `sep2/remove-node-friend-access`.

## 1. Goal

Remove every `friend` grant that gives NODE code access to wallet private
state. After this program, no class defined outside the wallet's own
sources appears in a `friend` declaration in `include/wallet/**` or
`src/wallet/**`, and a gate (F1) enforces that permanently.

## 2. Measured scope (stage 0, 2026-11-09)

Wallet headers hold 57 `friend class` declarations. Only one befriended
class is node-side: **`ChainstateService`**, defined in
`src/daemon/services/chainstate_service.cpp` (29,379 lines, `dinero_core`).

- `RuntimeOrdinaryDelivery` and `RuntimeWalletRecovery` looked node-side in
  §12's original list but are wallet classes: defined in
  `src/wallet/runtime_index_delivery.cpp` / `runtime_wallet_recovery.cpp`,
  compiled into `dinero_orchard_index_delivery`
  (`rust/orchard_backend/CMakeLists.txt:367-370`). `chainstate_service.cpp`
  only calls their statics. Their 7 grants are wallet-internal: out of scope.
- All other befriended classes are wallet-internal: out of scope.

**The 12 `ChainstateService` grants** (11 effective, 1 duplicate); every
access site is in `chainstate_service.cpp` ("CS"):

| # | Grant | Usage | Writes | Orchard |
|---|---|---|---|---|
| 1 | `authenticated_shield_inputs.h:16` | private ctor ×2 (CS:22476, 22609) | mint | gated |
| 2 | `orchard_account_catalog.h:63` | `StagePreactivation` (CS:7257), `RequireUnknownEmpty` (CS:7297) | 1 | gated |
| 3 | `orchard_account_delivery.h:372` | 37 sites, 19 private members (prepare/commit/sign split across node locks) | 9 | gated |
| 4 | `orchard_account_delivery.h:443` | **duplicate of #3** — deletable alone | — | — |
| 5 | `runtime_account_replay.h:52` | 7 factory calls (CS:7376-7547, 21295); inputs already node-neutral | 0 | gated |
| 6 | `runtime_index_delivery.h:59` | 13 private static calls on `RuntimeOrdinaryDelivery` (CaptureOriginDomain, CheckOriginDomain, Prepare/Reconcile/Advance Coverage, AdoptOrigin, ExistingCoverage, ReadCoverageBalance) | 4 | gated |
| 7 | `runtime_origin_projection.h:39` | `first_` w+r, `AppendValidated`, `imported_cohort_` w+r (CS:7571-7658, 21525-21620) | yes | gated |
| 8 | `runtime_origin_projection.h:99` | private `Build` (CS:7696), `replay_` reads ×8 | mint | gated |
| 9 | `runtime_wallet_balance.h:15` | private ctor ×1 (CS:21630) | mint | gated |
| 10 | `selected_history.h:15` | 29 sites: private ctor + all fields (CS:15023-15235) — class has zero public members | yes | **UNCOND** |
| 11 | `wallet_manager.h:1138` | `database_leases_` reads ×10, `loadInitialPqMaster` ×2, `RescanBlockchainImpl` ×2, `DiscoverSnapshotCandidates` ×1 | 3 | mixed (5 UNCOND) |
| 12 | `wallet_transaction_signer.h:41` | `SignBeforeActivation` ×2 (CS:7454, 7457) | sign+commit | **UNCOND** |

Unused grants: **none** (only #4 is free).

Cross-cutting constraints:
- **UNCOND sites** (grants 10, 12, part of 11) must build with Orchard OFF:
  their replacement APIs live in `dinero_wallet` or are header-only — never
  in `dinero_orchard_index_delivery`.
- **Node-owned SQL transactions:** CS:7241-7259, 7289-7305 and 21357-21362
  run raw `BEGIN IMMEDIATE`/`COMMIT` on the wallet handle. Replacements for
  `StagePreactivation`, `RequireUnknownEmpty` and
  `RecheckCatalogRecoveryInTransaction` must own the transaction (or accept
  a wallet-issued transaction scope), not inherit raw SQL from the node.
- `scripts/ci/separation/design_anchors.tsv:176-178` pins the
  `wallet_manager.h` friend lines; the TSV is updated in the same PR that
  moves or removes a pinned line.
- `chainstate_service.cpp` is compiled directly into 5 test executables
  (`tests/CMakeLists.txt:11194, 11266, 11341, 11889, 12100`); they must keep
  building at every stage.

## 3. Replacement principle

**Zero friend lines, including passkeys.** Each private access becomes a
public wallet API that validates its *inputs* instead of trusting its
*caller*: factories/builders for mint-only grants (#1, #8, #9, #10),
getters for read-only field access (#7, #8, #11 leases), and
split prepare/commit APIs that take an explicit source-fence callback where
the node interleaves its own locks (#3, #12, #11 rescan). Where today only
the node can construct a value, the replacement factory takes the same
capability objects the node already holds (chain contexts, session handles)
— restricting by evidence, not identity. A passkey/badge type would keep a
`friend ChainstateService` line alive and is rejected unless a concrete API
proves impossible to validate by inputs; using one requires an owner
decision, recorded here.

**No behavior change.** Signing, staging, recovery, rescan and coverage
semantics are byte-for-byte preserved; the regression net is the existing
test surface that drives these paths through public `ChainstateService`
methods (snapshot_wallet_recovery_checks.h ×22, test_assumeutxo_replay.cpp
×7, wallet_script_coverage_checks.h, orchard_origin_capture_checks.h, the
orchard_*_checks.h family, wallet_shield_*_checks.h,
snapshot_payment_source_checks.h).

## 4. Gate F1

`scripts/ci/separation/check_node_friends.py`: fails if any `friend`
declaration in `include/wallet/**` or `src/wallet/**` names a deny-listed
node-side class (`ChainstateService`, qualified or not; list extensible).
Prints `GATE F1 PASS` / `GATE F1 FAIL rc=1 (<file:line> grants <class>)`.
- Added in stage A, wired into the separation-gates workflows as a
  non-required job, and **observed RED** (listing the 11 remaining grants)
  before any migration lands.
- Neuter-proof: a pytest in `scripts/ci/separation/tests/` asserts the
  checker flags a synthetic in-memory header with a `friend class
  dinero::ChainstateService;` line and passes a clean one.
- Goes GREEN only in stage F, then becomes required.

## 5. Stages

Each stage is its own PR onto `dinero-main`, green on existing CI plus the
separation gates, Orchard ON and OFF, before the next starts. Logs grepped;
every new API ships with a test that fails without it.

| Stage | Content | Drops grants | Exit |
|---|---|---|---|
| A | Delete duplicate `orchard_account_delivery.h:443`; add F1 script + tests + non-required CI job | #4 | F1 runs, observed RED with exactly 11 findings |
| B | `RuntimeAccountReplay` public factories; wallet-side mint for `RuntimeWalletBalanceSnapshot` (via coverage-balance read); public free function wrapping `SignBeforeActivation` in `dinero_wallet` | #5, #9, #12 | F1 RED = 8; UNCOND paths build Orchard OFF |
| C | `SelectedWalletHistory` public builder; `rescanBlockchain` overload taking history+session; public `DiscoverSnapshotCandidates`; lease-count check API; `loadInitialPqMaster` replacement with identical nullopt semantics | #10, #11 | F1 RED = 6; assumeutxo replay + snapshot recovery tests green |
| D | Origin/coverage projection getters + builders (`ImportedCohort()`, `Replay()`, validated append/build); public capability-arg forms of the 8 `RuntimeOrdinaryDelivery` statics | #6, #7, #8 | F1 RED = 3 |
| E | `OrchardAccountDelivery` split APIs with source-fence callbacks (shield reserve/queue/finalize, recovered-account-0, ordinary-payment replay, catalog finalization); `AuthenticatedShieldInputs` factory; transaction-owning `StagePreactivation` / `RequireUnknownEmpty` | #1, #2, #3 | F1 RED = 0 at head, GREEN observed |
| F | Remove the deny-listed names from any remaining comments/anchors; F1 becomes required (stand-in skip job included, per the #890/#891 matrix-skip lesson) | — | F1 GREEN + required; final report |

## 6. Decision points flagged for the owner (not folded in)

- **F2 (later):** whether wallet-internal friends (OrchardAccountDelivery
  et al.) should also be reduced. Out of scope here.
- **Passkey fallback:** only if some API in stage D/E cannot validate by
  inputs; none identified yet.
- The stale §12 list in the old plan (`2026-10-29-wallet-node-separation.md:676-691`)
  is superseded by §2 above; the old doc is history and is not edited.
