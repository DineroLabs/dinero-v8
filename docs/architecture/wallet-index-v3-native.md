# Wallet index v3: native wallets (issue #939)

Status: native v3 route merged; creation-time three-address completion is in
`codex/v824-native-three-lane-create` for v8.2.4 qualification.

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
- **Phase 1 as built: Taproot pool only.** P2MR keys live in the separate
  `V7P2MRStore` SQLite file, which cannot join Begin's wallet transaction
  (WAL gives no cross-file atomicity). `ValidateRecoveryInventoryInTransaction`
  enforces the store ↔ watch-row bijection in both directions, so a crash
  between the two writes would leave every later validation failing. P2MR pool
  rows need an idempotent adopt-on-retry step first. A P2MR address issued
  before enrollment is still inside the covered domain.
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

#### Phase 2 as built

- **Pool table:** `native_v3_address_pool(script_pubkey PK, address, chain,
  idx, generation, issued, label, issued_at)`. It is unguarded. Change rows
  (`chain=1`) can never be issued (a CHECK constraint).
- **Seeding:** native Begin creates the table in its own transaction as
  generation 0. Receive rows that predate enrollment (index 0, anything
  already shown) are `issued=1` and keep their label.
- **Durable state:** native `pending` is reused, plus the pool generation. No
  new state value is added.
  - `complete`: the present ordinary receipt sealed the whole domain. Any
    unissued receive row may be issued.
  - `pending` with `MAX(generation)=0`: first enrollment. Nothing is issuable.
  - `pending` with `MAX(generation)=g>0`: an extension is in flight. Rows with
    generation `< g` were sealed by the receipt the extension required, so
    they stay issuable. Generation `g` is not issuable until the reconcile
    returns the wallet to `complete`.
- **Extension (`BeginNativePoolExtensionV3`):** one FULL transaction that
  needs the unlocked seed.
  - It requires native `complete` with a present ordinary receipt.
  - It derives the next 20 scripts of each low chain as ordinary issued rows
    and inserts them as generation `g`.
  - It authenticates the inventory and flips the state to `pending`. The
    guarded writes null the ordinary receipt in that same transaction.
  - It never touches `pending_payment_owner`.
- **Low water:** a chain is low when it has fewer than 5 unissued receive rows,
  or fewer than 5 change rows with no coin. A native-complete wallet without
  the table (enrolled before phase 2) adopts every row as issued, then extends.
- **Driver:** `resumeRuntimeWalletRecoveryV3IfNeeded`, called by the runtime
  worker, only when the wallet is unlocked.
  - A refused extension rolls back whole and Complete delivery continues.
  - A locked wallet keeps per-block delivery and extends after its next unlock.
- **Index receipt:** the DNUI03 receipt still names the pre-extension domain,
  and coverage decoding refuses it. Before its index commit, the native
  reconcile retires exactly that receipt.
  - It acts only if the wallet is `pending`, `g>0`, and the recorded domain is
    the current domain minus generation `g`.
  - It keeps a receipt over the current domain (an earlier interrupted
    reconcile).
  - It fails closed on any other domain.
  - The coverage reconcile then re-proves every index row.
- **Pending payments:** the native reconcile already applies the v2
  reservation rules (inputs unique, inside proved history, no Orchard
  overlap) and leaves the `send` row alone unless it confirmed.
- **Refusals (`getnewaddress`):**
  - `wallet_native_address_pool_extending`: retryable; every sealed row is
    issued and an extension is due or running.
  - `wallet_native_address_pool_unlock_required`: not retryable; the wallet
    is locked, so it cannot extend.
  - `wallet_native_p2mr_unavailable_on_v3`: for P2MR.
- **P2MR stays out of the pool.** `V7P2MRStore` is a separate SQLite file that
  cannot join the extension's wallet transaction, and
  `ValidateRecoveryInventoryInTransaction` enforces the store ↔ `watch_scripts`
  bijection both ways.
  - A crash between the two writes would leave every later validation
    failing, so the wallet could never reseal.
  - A crash-safe design would need a write-ahead intent in the wallet, plus
    idempotent adopt/discard of orphan store rows inside the validator.
  - P2MR derivation also needs a loaded PQ master, which these wallets often
    lack.
  - Until that exists, P2MR is refused honestly.

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

## Creation-time three-address route for v8.2.4

`wallet.createhd` returns Taproot, P2MR, and Orchard account-0 receive
addresses on v3. It reports `addresses_ready` only when all three were issued;
an encrypted new wallet is unlocked for issuance and relocked before the RPC
returns. Generated unencrypted wallets authenticate the PQ master sealed in
their initial owner instead of requiring a password wrapper.

P2MR issuance commits an exact wallet-side intent before writing the separate
key store. Only an authenticated key row matching that intent can be adopted
into `watch_scripts`; the intent is then cleared under the wallet transaction.
An interrupted issuance stays hidden until exact repair on unlock or inventory
validation. The initial P2MR script is present before v3 enrollment so the
ordinary receipt covers it.

The v3 transparent signer authenticates Orchard ownership and rejects inputs
reserved by Orchard before staging a payment. Balance and status only claim
native send readiness when the Orchard inventory is valid; a locked wallet
cannot authenticate that inventory and reports send readiness after unlock.
Orchard spends and
restored-wallet enrollment remain separate #939 work. Additional P2MR
addresses after v3 enrollment still need the P2MR pool and extension protocol
described above; no new unsealed P2MR script is issued on that route.
