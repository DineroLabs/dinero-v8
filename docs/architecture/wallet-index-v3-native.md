# Wallet index v3: native wallets (issue #939)

Status: design for v8.2.4. Owner-directed, owned by Claude, reviewed by codex-chainstate.

## Problem

`wallet-index-v3.md` defines v3 delivery for **legacy** transparent wallets only.
- **The gap:** every wallet that 8.2.x creates or restores has an Orchard owner
  (`wallet_initial_owner_v1`, `orchard_account_catalog_v1`).
  `BeginLegacyTransparentMigrationV3` refuses such wallets, the worker
  bypasses Orchard recovery on v3, and reads refuse every unenrolled wallet.
- **Effect:** on an operator-upgraded v3 node, a native wallet is never usable.
- **Interim (PR on `claude/v824-native-v3-guard`):** the node says so honestly
  and refuses to create or restore wallets on v3 nodes.

**Why the legacy route can't simply be widened:**
- **The state is a class marker.** `runtime_legacy_transparent_state_v1` acts as
  a wallet-class marker. `RequireOrchardWalletReady` throws for any non-Absent
  state, and Complete is terminal. Sending a native wallet through the legacy
  route would permanently lock out its Orchard side.
- **Missing checks.** The legacy v3 reconcile also lacks the v2 reconcile's
  catalog-capture recheck and the Orchard/transparent reservation-overlap check.

## Design

### Phase 1: native transparent enrollment

- **Separate durable class:**
  - **Column:** `wallet_meta.runtime_native_v3_state_v1 ∈ {pending, complete}`.
    The legacy column stays absent, so `LegacyTransparentState()` stays Absent
    and Orchard readiness is unaffected.
  - **Eligibility:** only wallets with Orchard owner state may enter. Legacy
    wallets keep the legacy route unchanged.
- **Begin (atomic, requires the unlocked seed):**
  - **Validate:** `ValidateRecoveryInventoryInTransaction`, which already handles
    owner material through `loadInitialPqMaster`.
  - **Pre-derive the receive pool:** Taproot external 0..N-1 and P2MR 0..N-1, as
    ordinary `addresses`/`watch_scripts` rows plus P2MR store rows. N = 20, the
    existing gap limit.
  - **Commit together:** set `pending` and keep the existing delivery ID in the
    same transaction.
- **Coverage:** `getRuntimeWalletCoverageV3`, unchanged. The domain includes the
  pool, so one replay proves the history of every pool script. The **sealed
  fresh birth** (generated identity, birth ≥ activation) skips the
  pre-activation replay. A restored wallet always gets the full replay.
- **Reconcile:** a native variant of `ReconcileLegacyTransparentCoverageV3`:
  - Same two-phase commit: index receipt, then ordinary receipt, with Pending
    fail-closed in between.
  - Adds the catalog-capture recheck and the reservation-overlap check from the
    v2 `ReconcileCoverage`.
  - Never refuses because of Orchard reservations; it checks them for overlap.
  - Marks `complete`.
- **Per-block delivery:** the worker's Complete branch (`ApplyForWalletV3` plus
  ordinary delivery) also serves native-complete.
- **Reads:**
  - Native-complete uses the same v3 receipt fence as legacy-complete.
  - It is **not** stamped `legacy_transparent_only`: native wallets keep their
    full wallet scope.
  - Transparent sends use the existing recovered-v3 send path. Change goes to a
    pooled Taproot change address.

### Phase 2: issuing addresses from the pool

- **Pool table:** `native_v3_address_pool(script PRIMARY KEY, kind, idx, issued, label)`.
  - The table is **unguarded**, deliberately outside the `OrdinaryGuards` tables.
  - Issuing flips `issued` and stores the label there. It never touches the
    guarded `addresses`/`watch_scripts`, so receipts stay valid.
- **Issuance:** `getnewaddress` / `getnewp2mraddress` on a native-complete
  wallet hand out the next unissued pooled script of the requested kind.
- **Exhaustion:** with fewer than 5 unissued scripts of a kind left, the wallet
  enters `extending`:
  - Derive the next 20 as Pending.
  - Re-run coverage. That's cheap with a fresh birth, and a full replay for
    restored wallets.
  - Reconcile, then return to `complete`.
  - Issuance refuses honestly only if the pool is fully exhausted mid-extension.
- **Invariant:** a script is never handed out unless a sealed receipt's replay
  covered it.

### Phase 3: Orchard delivery on v3

- **Relax the binding:** `getRuntimeAccountReplayForWallet` currently requires
  `wallet.getUTXOIndex()==utxo_index_`. Relax it to also accept a metadata-only
  v3 source when the wallet's index is null.
- **New worker step:** after `ApplyForWalletV3`, a v3 Orchard step modeled on
  `ResumePreparedOrchardCatalog`:
  - No index writes.
  - A selected-lock plus prefix fence.
  - The revision is rechecked inside each commit.
  - It never opens the legacy shielded sidecar.
- **Create-time Orchard account 0:** becomes available here.

## Compatibility matrix

| Reader | Native-v3 wallet file (phase 1+) | Result |
|---|---|---|
| 8.2.3 on a v3 node | Legacy column absent → Absent | Refused as "catching up" (no worse than today). New column ignored. |
| 8.2.4 interim build | Absent with Orchard owner | `wallet_native_unsupported_on_v3` (honest). |
| v2 full-archive node (8.2.x) | Orchard owner, legacy Absent | Normal v2 native route (`ResumeCatalogWalletStores`). An ordinary receipt invalidated by triggers just forces re-coverage. Pool rows are ordinary addresses. |
| Phones (compact v2) | Not exchanged: phones restore from seed | Unaffected. |
| Legacy wallets on v3 | Unchanged route | Unchanged. |

## Safety invariants

1. **Atomic enrollment:** ID and state become durable together, and Pending is
   fail-closed.
2. **Commit order:** index receipt first, then ordinary receipt. Height is
   published only after both.
3. **Lock order:** selected → wallet → index. Every `wallet_utxos` statement
   binds `wallet_identity`.
4. **Fresh-birth skip:** only for a sealed Generated identity with
   birth ≥ activation, verified with the unlocked seed. An unverifiable record
   fails closed.
5. **Unscanned scripts:** none is ever sealed or issued.
6. **Guarded tables:** issuance and labels never write `OrdinaryGuards` tables.
7. **P2MR:** the store ↔ watch-row bijection holds.
8. **Orchard:** the Orchard side is never locked out. Native wallets never write
   the legacy column.

## Tests (acceptance first)

1. **Native enrollment (generated):** a native generated wallet on a v3 index
   with a body present → unlock → enrolls Complete. The balance is current, and
   the wallet scope is not legacy-only.
2. **Fresh-birth discriminator:** the same as test 1 with a pre-activation body
   deleted, so it can only pass via the fresh-birth skip. A restored native
   wallet with the same deletion → Deferred, because it needs the full replay.
3. **Issuance after Complete:** a Taproot and a P2MR address come from the pool,
   and the receipts stay valid. Funds sent to a pooled address are delivered.
   Survives a restart.
4. **Legacy still legacy:** a legacy wallet keeps the legacy route.
5. **Pool extension:** exhaustion leads to an extension, back to Complete, and
   issuance continues.
6. **Orchard:** a native-complete wallet keeps Orchard readiness. In phase 3,
   Orchard account delivery advances on v3.
