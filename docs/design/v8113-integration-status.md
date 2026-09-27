## Consistent authenticated P2MR key reads (2026-09-27)

Actual P2MR signing and export now capture metadata/ciphertext together and
require seed/public-key/stored-root/requested-address agreement before returning
signatures or plaintext. SQL and borrowed-transaction errors refuse explicitly;
existing addresses and metadata are preserved. See [scope and validation](wallet-p2mr-key-read-2026-09-27.md).
This remains a single-record component: complete discovery, explicit key
initialization versus recovery, staged unlock and all release gates stay open.

## Header recovery target linkage (2026-09-27)

Full, QUIC and core-heavy Linux builds exposed a missing test-only mempool
link dependency in the archival header recovery target. The target now uses
the existing isolation source, and the independent Orchard workflow builds
and executes this regression with retained inventory and logs. See
[scope and qualification](header-recovery-link-2026-09-27.md). Production
behavior and release gates are unchanged; repaired-source Linux results remain
required.

## Checked P2MR inventory (2026-09-27)

P2MR listing now refuses incomplete or malformed reads, and its JSON listing
adapter opens existing storage read-only. This is scoped present-row validation,
not cryptographic ownership or a complete key/account catalog; it cannot justify
creating a missing master. See [scope and validation](wallet-p2mr-inventory-2026-09-27.md).
Unlock, reconciliation, pending ownership, consumers and release gates remain open.

## Checked ordinary address issuance (2026-09-27)

## 2026-09-27: coherent explicit Taproot import persistence

The actual descriptor RPC now uses a session-bound wallet/SQLite owner to commit
its key, imported address, public mapping and watch registration together. See
[the import owner scope](wallet-taproot-import-owner-2026-09-27.md). This advances
forward imported-key persistence only. Complete discovery/reconciliation,
imported-key signing/readmission, provider installation, first activation and
steps 1–4 remain open. Mainnet activation remains unset.


Receive and change issuance now commit address/path/watch records in one checked
FULL transaction under the existing wallet owner. Required writes, commit and
next-index range are checked; caller transactions and conflicting watch ownership
refuse. Live index registration follows commit. Actual backend-ON/OFF daemon
builds and affected tests, fresh 71-linked-C++ sanitizers and fault controls pass.
A new required CI lane executes five issuance cases. Existing incomplete rows,
authenticated inventory, full discovery and provider work remain open; see
[scope and qualification](wallet-address-issuance-2026-09-27.md).

## Same-block originated history (2026-09-26)

The actual service fixture now qualifies signed parent/child spends within the
first canonical event: missing child metadata refuses before either receipt,
ordered adoption survives reopen, and failures after parent effects or parent
unconfirmation roll back ordinary state. Actual undo/reconnect preserves both
send records. Production code is unchanged. Fresh declared builds, full daemon,
three CTests, all 203 linked project C++ sanitizer units and two copied controls
pass. Exact Linux remains pending. This does not complete discovery, pending
ownership or provider integration; see [scope](wallet-origin-same-block-2026-09-26.md).

## First-event history preflight (2026-09-26)

Known-script origin adoption now refuses missing originating history for an
owned spend in actual event 1 before the independent index commits. The writer
and first-event retry also check it. No categories, fees or pending ownership are
inferred. The full daemon, three affected CTests, fresh 203-project-C++ sanitizer
build and copied controls pass. Exact Linux qualification remains pending; see
[scope](wallet-origin-first-history-2026-09-26.md).

## Signed-spend recovery and history undo (2026-09-26)

A new actual service case covers independently replayed signed pre-origin spends,
first-event owned change spends, existing send metadata and ordinary undo. It
exposed deletion of the existing send history on disconnect. Checked ordinary
recovery now unconfirms that history using exact owned input/spender facts in the
same transaction as coin undo and the existing receipt. The full daemon, three
affected CTests, fresh 203-project-C++ sanitizer build and copied controls pass.
Exact Linux qualification remains pending. See [scope and checks](wallet-origin-signed-spends-2026-09-26.md).

## Optional backend build correction (2026-09-26)

The backend-OFF full/QUIC PR builds exposed unconditional Orchard header imports.
The service and replay tests now follow the actual optional implementation
boundary, with a disabled-backend API refusal test and generated-command checks.
Earlier macro-removal checks retained backend include paths and did not qualify
a complete OFF configuration. See [scope correction and verification](orchard-backend-boundary-2026-09-26.md).
Fresh backend-OFF and backend-ON daemon builds and affected tests pass, with
explicit ten/eleven replay-case inventories. The actual service fixture passed
with all 203 linked project C++ translation units freshly instrumented. Exact
Linux full/QUIC checks are still required; recovery and release gates remain open.

## Known-script origin adoption (2026-09-26)

Actual service capture now includes optional index scripts/paths and database path
alongside the ordinary domain. A new explicit service adoption operation applies
the known-script origin baseline and real event 1, index first then ordinary,
using existing DNUI01/DNOW01 receipts. Partial commits retry through the existing
prefix; receipt-only baselines are checked against source coin inventory. Missing
originated history, conflicting/orphan rows and invalidated stores still refuse.
No startup/RPC/provider installation or first-activation bootstrap is claimed.
See [scope and qualification](wallet-origin-adoption-2026-09-26.md).

# v8.1.13 integration status

## Outbox-origin ordinary facts (2026-09-26)

The service can prepare projected ordinary creations, spends and exact relevant
transaction facts from checked-origin ancestry using owned historical replay. See
`wallet-origin-projection-2026-09-26.md`. The successful service path is now
exercised over a short independently replayed regtest ancestry and actual indexed
boundary (see `wallet-origin-capture-qualification-2026-09-26.md`). Ordered
index/ordinary adoption remains unqualified; local sends/pending state,
CT epochs, discovery, provider installation and steps 1–4 remain open.

## Selected wallet history source (2026-09-26)

A service-owned archival source now validates captured genesis-to-tip bodies before
wallet ownership and supplies immutable data to the checked ordinary rescan. See
`wallet-selected-history-source-2026-09-26.md`. It is bounded, pre-Orchard and
stateful; it is not installed in the rescan RPC or recovery provider. Complete
outbox-origin projection, CT/history/discovery, long-history support and baseline
adoption remain required. Steps 1–4 are not complete.

## Checked ordinary block rescan (2026-09-26)

The existing wallet block-rescan path now commits required ordinary effects,
progress and persisted tip together, refuses SQL failures and borrowed ownership,
and publishes height after COMMIT. See `wallet-block-rescan-2026-09-26.md`.
Its mutable archival source, ordinary history/CT reconstruction and complete
ownership discovery still prevent using it as the transparent recovery baseline.


## Owned historical replay (2026-09-26)

The production background replay now binds genesis and block identity, owns its
header ancestry, and resolves contextual time locks from that same ancestry.
See `owned-historical-replay-2026-09-26.md`. This prepares the historical source
needed for transparent baseline reconciliation; baseline adoption, provider
installation and full activation/startup qualification remain incomplete.

## Snapshot wallet import (2026-09-26)

The existing snapshot import now pins wallet ownership and commits owned outputs,
schema and scan progress together in a checked FULL transaction. The configured
file producer refuses incomplete entry delivery. See
`wallet-snapshot-import-2026-09-26.md`. Source provenance, complete transparent
pre-origin reconciliation and production provider installation remain open.

## RPC listener startup (2026-09-26)

RPC startup now acquires the actual listening socket before reporting success.
The required actual-daemon regression covers occupied-port refusal, reopening
its isolated datadir, authenticated RPC and clean shutdown. See
`rpc-listener-startup-2026-09-26.md`. This does not complete the recovery/provider
or activation-history requirements below.

## Authenticated archive inventory (2026-09-26)

Enrolled-account recovery now recognizes completed-operation records through
actual authenticated archive links in the shared snapshot table, refuses
unclaimed/missing/corrupt rows, and rechecks archive revisions during recovery.
See `wallet-archive-inventory-recovery-2026-09-26.md`. This does not certify an
account-complete baseline. Ordered ancestor reactivation is described below.

## Archived operation reactivation (2026-09-26)

Coordinated account delivery now restores archived pending reservations during
ordered undo in the same transaction as account effects and source progress.
Already-applied ancestor checkpoints are reconciled too. Immutable branch
identity checks refuse missing ancestry. Replacement-branch observation replay
now uses actual bodies from the covered common ancestor; see
`wallet-archive-reactivation-2026-09-26.md` and
`wallet-archive-branch-replay-2026-09-26.md`.
This does not install a provider or complete baseline/history qualification.

## Account origin recovery (2026-09-26)

Existing authenticated accounts with an empty scanner at the exact checked
source origin can now join coordinated recovery. Late accounts and explicit
rescan results earn their first receipt by scanning the real first event;
notes, observations and retained parent commit together. No missing account or
ordinary baseline is created. See `wallet-account-origin-recovery-2026-09-26.md`.

## Current recovery integration (2026-09-26)

The service-owned immutable branch replay view and authenticated parent-revision
locators are implemented. The existing index, ordinary wallet and bound Orchard
account consumers share an ordered coordinator. The new enrolled-account entry
point discovers and fully restores all current account snapshot rows, checks
all applied source positions and retries partial commits across accounts. See
`wallet-account-coordinated-recovery-2026-09-26.md` and
`wallet-enrolled-account-recovery-2026-09-26.md`.

These APIs are compiled into the daemon but not installed notification/startup
callers. Transparent baseline/pre-origin reconciliation, account creation/key
discovery, authenticated inventory
completeness, general long-history operation, remaining configured consumers,
production installation and independent activation/lifecycle qualification
remain open. Earlier component notes below describe their original scopes;
account integration and automatic parent locators are no longer missing.

- A bound Orchard account consumer now pins the selected wallet keys, applies
  account effects and retains authenticated parent snapshots in one SQLite
  commit. Key changes serialize with recovery; missing parent history refuses.
  Service-owned replay views/parent locators, coordinator integration and
  production installation remain open. See
  `orchard-bound-account-recovery-2026-09-26.md`.

- Orchard account delivery now follows exact historical blocks below activation,
  moving an empty scan checkpoint and applying/undoing real pending-input
  conflicts with its existing receipt. DNORAC05 restores those conflicts from
  selected historical bodies; versions01–04 remain readable. This does not
  install the account owner/provider or certify historical consensus. See
  `orchard-account-historical-delivery-2026-09-26.md`.

- Transparent store recovery now connects checked service pages to existing
  index and ordinary receipts, validates both applied source positions, and
  resumes ordered partial commits through a captured head across bounded pages.
  Missing baselines refuse; the result covers these stores only. Account/note
  integration, all-consumer readiness and production installation remain open.
  See `wallet-store-recovery-2026-09-26.md`.

- Ordinary wallet source delivery now commits real typed UTXO/history effects,
  derived metadata and a per-store cursor together under the persistent wallet
  lease. Ordered retry can distinguish a committed index prefix from pending
  ordinary effects. See `wallet-ordinary-delivery-2026-09-26.md`. Production
  all-store recovery, account/note integration and first-boundary history remain
  unfinished; this is not an installed notification provider or activation.

This branch collects release implementation for review and qualification against
`dinero-main`. It is a draft candidate, not a release or an activation decision.

## Included source

- Public index recovery entry points now pin the actual WalletManager lease,
  reject stale process sessions, and use a persistent database ID committed
  before index effects. Caller-supplied identity methods are private. Reopen,
  same-name different wallets and a switch during blocked index access are
  covered. This still needs validated wallet/script baselines and coordinated
  progress across the other stores; no provider is installed. See
  `wallet-delivery-binding-2026-09-26.md`.

- A store-owned index delivery consumer applies real historical/Orchard
  transparent effects and ordered source progress in one checked SQLite
  transaction. Ordinary index writes invalidate that progress atomically;
  resets preserve invalidation, including empty indexes. Checked source,
  persistent wallet binding and pre-origin baseline reconciliation remain
  caller obligations. This is one index consumer, not an installed production
  provider or all-wallet readiness. See `orchard-index-delivery-2026-09-26.md`.

- The existing delivery reader now binds its head (including EOF) to the durable
  canonical tip and checks visited adjacent transition identities. The service
  exposes checked pages under its activation lock and its actual startup gate
  refuses inconsistent retained delivery history even below activation. This is
  source verification, not per-store completion or an installed recovery
  provider. See `orchard-delivery-source-2026-09-26.md`.

- Existing wallet connect/disconnect/reorg queues now bind a process-local
  wallet session at enqueue and compare it under the processing lease before
  any store effect. Replacement and same-name reopen invalidate old jobs; empty
  selection cannot retarget work. This is transient ownership, not a source
  cursor or durable recovery acknowledgment. See
  `wallet-queued-identity-2026-09-26.md`.

- Ordinary WalletManager disconnect now groups output deletion, history removal
  and input restoration in one checked SQL transaction under the database lease.
  Failure leaves ordinary rows and height unchanged; the separate index may have
  already rolled back and needs ordered retry. The actual worker refuses invalid
  heights and nested active wallet transactions before touching that index. An
  independent real-worker CTest covers failure/retry/reopen and is required by CI.
  This does not complete cross-store recovery. See
  `wallet-ordinary-disconnect-2026-09-26.md`.

- The existing WalletWorker now groups ordinary WalletManager block writes in a
  checked SQLite transaction, distinguishes confirmation lookup from SQL failure,
  and propagates required write failures. Ordinary creation replay preserves
  recorded spends. The index commits first; a later ordinary-wallet commit failure
  leaves a prefix requiring ordered replay and does not advance worker height.
  Real two-store worker tests cover failure/retry/reopen. This is not a durable
  source cursor or complete recovery provider. See
  `wallet-ordinary-block-atomicity-2026-09-26.md`.

- The existing WalletWorker connect path now commits a block's UTXO-index changes
  together, checks mutation failures and defers height/vault observations until
  index commit. Real-worker failure/retry/same-block-spend tests and real SQLite
  transaction ownership tests cover this path. WalletManager and note databases
  remain separate; this is not the complete durable recovery provider. See
  `wallet-block-index-atomicity-2026-09-26.md`.

- Existing wallet worker connect/disconnect/reorg and synchronous rescan now pin
  the selected WalletManager database and serialize its SQLite connection for
  the whole job. Wallet create/open/close cannot replace that database during a
  lease; reentrant switches refuse. Borrowed transactions are not adopted, and
  unfinished leased transactions roll back before the last owner releases. All
  legacy shielded runtime entry points now pin the wallet before their shared
  runtime lock; nested scans preserve the owning rescan transaction. This establishes
  connection ownership, not atomic commits across wallet stores or durable
  delivery completion. See `wallet-database-ownership-2026-09-26.md`.

- The actual wallet UTXO index rollback now checks transaction acquisition,
  deletion, un-spending and commit. Any SQL failure aborts its own transaction;
  nested caller transactions are refused without being committed or rolled back.
  A mandatory real-SQLite regression injects failures at both statements and
  commit, then checks retry/reopen. Creation replay now preserves a recorded
  spend height, including after reopen; explicit undo still restores unspent
  outputs. Real index regressions cover stale replay and import compatibility.
  This repairs existing consumer prerequisites;
  it does not install the complete Orchard recovery provider.

- Orchard account snapshots now bind a delivery sequence/digest to actual scan
  advancement or immediate-parent rollback. The receipt shares the encrypted
  payload and SQLite commit with notes, pending operations and address counters.
  Fresh-process exits before/after commit cover state/receipt atomicity. This is
  account-side delivery support, not an installed production wallet provider:
  source verification, transparent wallet effects, historical events below
  activation and rescan reconciliation still need the owning recovery service.
  See `orchard-wallet-delivery-2026-09-26.md`.

- Miner longpoll now observes actual shared tip publication, including rollback
  and same-height branch replacement. It precedes fallible downstream callbacks
  and is silent on an unchanged identity. RPC waiting captures generation before
  reading the tip, closing a missed-wake window. This is an installed built-in
  notification consumer; durable wallet and other consumer recovery remain open.

- Stateful historical connect/disconnect retains delivery records after an
  Orchard outbox origin, including transitions below activation. Preparation
  occurs before historical memory changes; the record joins the existing
  canonical batch. Unsupported runtime/CSN paths refuse a retained delivery
  history. This closes a source-level handoff gap, not production consumer
  recovery or whole-node reorg qualification. See the historical-delivery design.

- Pinned Orchard backend, immutable C++ signing context and bounded draft outer
  transaction envelope, with exact-source component and root-build CI jobs.
- Typed shared reader with historical-format regression tests and a default
  legacy-parser rejection boundary for marked Orchard envelopes.
- Transparent mempool reconciliation now accepts typed connected-block effects.
  The mixed-body adapter extracts actual Orchard and ordinary transaction IDs
  and transparent prevouts; the existing historical callback delegates to the
  same eviction/staleness path. Both callbacks remove the full unconfirmed
  descendant branch of a conflict while retaining children of confirmed
  transactions; replay leaves survivors intact. The focused test uses the real
  coin overlay and runs explicitly in the Orchard CI workflow. This is a prerequisite for production block
  notification, not Orchard admission: nullifier conflicts, wallet events,
  relay and production notification providers remain open.
- Daemon stored-body queries now have selected-height typed routing under the
  activation lock. The optional build can return exact mixed-body bytes from
  indexed flatfiles after identity checks; historical hex queries retain their
  old parser before activation. This is read-only query integration, not
  ConnectTip, admission or relay. See the runtime-reader design document.
- Owned consensus-view coin resolution and staged transparent authorization:
  restricted native Taproot/P2WPKH signatures bound to Orchard intent, maturity
  and contextual locks. No production admission caller is enabled.
- Combined transparent and Orchard authorization for one owned transaction and
  coin snapshot, with honest synthetic shield and cross-address spend fixtures.
  This verifies authorization, not anchor membership, unspentness at application
  time, or an atomic chainstate transition.
- ChainDB staging for Orchard block state, nullifier ownership, anchor references
  and undo, tested with companion coin/tip writes on generated RocksDB stores.
  Actual service routes now invoke staging through the indexed owner; activation
  history and production downstream consumers remain required.
- A daemon commit owner now holds the activation lock and a private full-state
  batch through synchronous write and prepared memory publication. Abandonment,
  single-use/thread checks and storage-error fail-stop behavior are covered on
  generated stores. No late batch writes are exposed. Production connector,
  active-tip and startup wiring remain unfinished; see the commit-owner design.
- Indexed commit preparation now fsyncs exact mixed-body and conventional undo
  records before staging their locators with the full chainstate batch. Memory
  availability flags publish after commit. Existing locators must read back the
  exact bytes; stale metadata aborts before writing. This prepares service-index
  coordination but does not enable production ConnectTip or claim validity flags.
  See the indexed-commit design and its isolated restart scope.
- The actual shared active-tip setter now publishes pointer and observer identity
  under the observer mutex before best-effort diagnostics. Allocation failure
  during logging cannot interrupt publication after a durable commit. The real
  service regression covers advancement, rollback and null-tip transitions with
  thread-local allocation refusal. Other post-commit consumers remain unfinished;
  see the service-tip-publication design.
- The actual activation-time startup journal gate now requires the Orchard
  tip-local consistency audit, exact indexed body/undo and restored-memory
  agreement. Failure enters safe mode and does not consume the one-shot gate.
  Historical journal behavior stays optional before activation. This is not yet
  complete startup/replay/reindex or production connection; see the service
  startup audit design and generated-store qualification scope.
- The actual DisconnectTip now has a typed Orchard route using the indexed
  atomic owner and parent-tip publication. It requires a prepared typed consumer
  event before committing, invalidates stale position-cache entries, and publishes
  the event after durable and in-memory state agree. Generated-store tests invoke
  the real service for descendant and activation-boundary rollback. No production
  notification provider is installed yet, so live Orchard rollback still refuses;
  Boundary connection and end-to-end integration remain unfinished. See the service
  disconnect design for this readiness condition and fixture scope.
- The actual ConnectTip now routes Orchard descendants through selected contextual
  header/work checks, mandatory parent audit, the indexed atomic owner and typed
  prepared notifications. Validity flags join the commit, then memory, active tip
  and events publish in order. First activation still requires a service-owned
  validated historical accounting source; no production notification provider is
  installed. Generated-state reconnect tests are not live-node qualification.
  See the service-connect design and its remaining readiness boundaries.
- ActivateBestChain now uses a shared locked fork-point forest check after
  disconnect. Selected Orchard forks require exact indexed mixed bodies and
  agreement of active/live/durable/validated/forest-marker identities; the live
  forest must match the authenticated body root. Historical parsing remains
  before activation. Dedicated service-fixture coverage verifies missing material,
  wrong restored forest and stale fork selection without canonical writes.
  Full reorg orchestration and production consumers remain unfinished.
- Best-chain activation and explicit invalidation now prepare complete typed
  reorg body plans before rollback. A consumer must durably retain the plan;
  per-block-only providers refuse. Completed plans report exact counts; interrupted
  plans require canonical recovery because legacy callbacks may fail after a
  durable write. Mixed bodies never enter the legacy transaction collector.
  The service now synchronously retains the whole intent and the outbox origin
  before returning readiness. Recovery reads exact typed plans without source
  flatfiles; cancelled attempts remain. The production recovery/readmission
  provider and consumer checkpoints remain unimplemented. See the reorg-plan
  and reorg-intent-store designs for bounds and qualification limits.
- Indexed Orchard transitions now append exact delivery records and a checked
  sequence head in the same synchronous chainstate batch. Rollback retains the
  history, and bounded readers check cursor/domain/body identity. This supplies
  durable per-block replay material; production consumers, per-consumer cursors,
  reorg-intent reconciliation and coordinated startup remain unfinished. See
  orchard-runtime-outbox-2026-09-26.md.
- The actual startup undo-coverage walk now routes retained Orchard bodies under
  the activation lock, restoring and reversing a private forest across the
  requested window. Exact indexed/embedded body and undo, commit records,
  coin/delta correspondence, filters and indexes are checked without database
  writes. Missing Orchard material enters safe mode instead of ending the old
  historical walk as success. This is bounded retained-material coverage, not
  historical consensus replay or stateless support; see the service undo-coverage
  design. The separate tip-local audit remains mandatory.
- The actual legacy persistence helpers now preserve frozen retirement state.
  Shutdown/notifications can confirm an unchanged cache without rewriting it;
  legacy marker rebinding and snapshot replacement refuse while retirement exists.
  This covers helper writes, not the complete snapshot/startup/Orchard connector.
- Independent transparent-value pool arithmetic enforced by the storage staging
  API, starting at zero, including fees, checked bounds and undo. Runtime block
  flow collection is still required; see the pool-guard design document.
- Immutable upstream Orchard commitment frontier with canonical bounded storage
  encoding, derived root/size and failure-atomic append.
- Sealed Orchard state preparation binds parent frontier/root/size, authorization
  context, selected-history anchors, nullifier freshness and ordered pool flows.
  A ChainDB adapter stages that result in the caller's batch and distinguishes
  local corruption/read errors from transaction rejection. Real frontier funding,
  spend, reopen, undo and branch replacement pass in temporary stores. This is
  still not wired into the daemon's mixed-transaction block connector.
- Typed mixed-block candidate reader with shared transaction/witness Merkle and
  DINW checks and base/weight accounting enforced before coin lookup. The ChainDB staging adapter requires exact, ordered authorization
  coverage of every Orchard transaction in that candidate and rejects retired
  legacy shielded transactions. This remains a staged format, not live admission.
- Contextual header component checks selected-parent height/linkage, network,
  MTP/future time, exact shared ASERT difficulty and proof of work using owned
  branch values. Competing timing-boundary branches are tested. Live admission
  still needs to invoke it and complete fork-choice/resource obligations. Configured checkpoints now bind to candidate ancestry, including competing branches; runtime replay/reindex routing remains unfinished.
- Selected-network Orchard activation/signing context is now explicit. All
  network defaults remain inactive; public schedules must match the joint
  release profile, and the staged header gate rejects caller context that
  disagrees with the selected height/branch. No runtime caller is enabled.
- Draft mixed-block resource accounting caps aggregate bundles/actions before coin lookup or proof verification, counts static scripts and resolved input signature work including same-block children, and returns charged usage. Runtime/miner callers and loaded-platform capacity qualification remain open.
- Ordered shared coin processing verifies Orchard and ordinary signatures,
  supports same-block children, prevents cross-family double spending and binds
  the coinbase limit to the same validated fees. Stateful ChainDB adapters stage
  coins, conventional undo and Orchard state together and reverse them after
  checking exact body/undo coverage. Synthetic connect/reopen/disconnect/reconnect
  tests pass; production service integration remains unfinished.
- Stateful mixed forest computation uses the actual transaction identities and
  creation-height leaf rules, excludes transient outputs, checks parent/header
  commitments and supports checked delta rollback on a private clone. A combined
  stateful adapter stages durable delta/checkpoint, forest/height/tip markers and
  coins/Orchard state in one batch; persistent-delta undo survives database reopen.
  The mixed compact filter is checked before commit and stored atomically; startup/disconnect rederive bytes and element count from body and checked undo.
  Peer proof targets, paths and ordered metadata are checked against resolved
  inputs and the authenticated full parent forest.
  Exact body storage and active transaction indexes now share that batch.
  A versioned commit record shares the batch; tip-local startup auditing and eight process-exit boundaries pass on generated stores. Production/CSN integration and service/flatfile-index coordination remain unfinished.
- Prepared in-memory publication checks the live coin/tip/root view and
  allocates changed-coin nodes before the durable batch. Connect/disconnect
  publication updates coins, forest and tip without C++ allocation after commit.
  Temporary-store integration and allocation-failure checks cover this primitive;
  ChainstateService still needs to own and invoke the complete sequence.
- Full staged disconnect returns a restart-safe reverse coin patch from checked
  body/undo, bound to the exact forest delta and restored parent leaves. Fresh
  processes rebuild the current memory view from storage and qualify both
  directions through pre-commit, post-commit and post-publication exits. This
  closes patch reconstruction for the staged adapter; runtime ownership and
  coordinated legacy state handling remain unfinished.
- Explicit staged DNRS v2 encoding is separate from the legacy v1 parser and
  lookup; existing callers and historical SHR1 behavior stay unchanged. Fixed
  wire vectors and rejection cases qualify the encoding boundary only. Composite
  root construction and retirement storage are staged below; authenticated
  retirement derivation and live enforcement remain open.
- Staged legacy retirement storage now records an ancestry-bound frozen receipt,
  advances the legacy tip marker atomically, and provides exact boundary undo.
  Synthetic-store tests cover Orchard companion batches, reopen and process exits.
  The full staged connector below binds it to DNRS and checks frozen contents.
  Historical monetary accounting and production runtime enforcement remain
  required; this does not retire a live pool.
- Draft composite state-root construction binds retirement/profile/ancestry,
  Orchard frontier/pool and canonical logical nullifier/anchor sets. Read-only
  ChainDB projection and independent Python vectors are tested. This scans
  existing sets. Reverse projection now checks stored undo, removed-nullifier
  ownership and restored anchor membership before writing rollback state.
  Full staged connect/disconnect now enforces current and parent DNRS v2,
  rederives the frozen legacy SHR1 from ChainDB contents, and includes retirement
  receipt/undo/marker writes in the same authoritative batch. Runtime callers,
  integration of selected-history boundary accounting and loaded capacity remain open.
- A read-only selected-history accounting adapter now derives the current legacy
  epoch's public pool flows from authenticated bodies and original prevouts,
  with explicit fees, reset handling and fail-closed incomplete/unknown amounts.
  A selected boundary factory now binds that result to reconstructed frozen
  contents and the selected parent's legacy DNRS where active. It requires
  independently validated archival history. Production callers, CSN/pruned
  accounting sources and complete supply reconciliation remain unfinished;
  this is not historical consensus replay.
- Wallet primitives now include opaque ZIP32 account keys, external/internal receivers, watch-only parity and an explicit-network Bech32m address profile. Fresh shield/send/unshield bundle construction uses OS randomness and an owned transaction signing context, then decodes and verifies before returning. Received notes are opaque; incremental witnesses check roots, preserve prior state and resume from a canonical checkpoint-bound encoding. No wallet database or RPC caller is enabled.
- Encrypted wallet snapshots stage in the caller-owned SQLite transaction with companion wallet records, checked revisions and process-exit recovery tests. Typed scan state now tracks receipts/spends and witnesses and restores from an encrypted snapshot against an exact selected checkpoint and authenticated origin callbacks. A selected archival ChainDB restore adapter now authenticates origins and resolves historical or ordered same-block prevouts from original bodies, with read-only reopen coverage. Indexed flatfile bodies now restore through the same typed authentication gate when the embedded copy is absent. Pruned/archive coverage, stale-checkpoint recovery, coordinated startup and live callers remain unfinished.
- Pending-operation component reserves inputs before proving, freezes fully authorized canonical bytes before broadcast, and restores encrypted Reserved/Ready states through process-exit boundaries. Live selection, service job integration, admission, broadcast and confirmation/reorg archival remain unfinished.
- Bounded proof executor stages one worker and four total jobs/results, checks
  exact durable reservation intent and discards cancelled active results. It is
  not wired into wallet RPCs; active proofs cannot be preempted and production
  shutdown drain time is not yet qualified.
- Account advancement records confirmation/conflict evidence for pending operations, including transparent-input and nullifier conflicts. Encrypted restore rechecks selected-block evidence; rewind/rescan reverses derived observations while retaining signed bytes and reservations. Encrypted completed-operation archival now stages history and pending removal atomically, retains exact signed bytes, and supports authenticated bounded pagination and selected-chain reactivation. Runtime reconciliation/backlog and live admission/rebroadcast remain unfinished.
- Account snapshots combine scanner, pending operations and durable external/internal address counters. Rewind and explicit rescan reset only derived scan state; they preserve address issuance and frozen pending transactions. These are staged wallet components, not live RPC callers.
- Pinned dependency advisory CI gate with saved reports and visible maintenance
  warnings; known vulnerabilities, unsoundness and yanks fail the gate.
- Empty-scriptSig envelope rule, host-aligned 100,000-byte ceiling and a shared
  outer/inner signing-profile identity.
- Explicit domain/profile checks across Rust/C++, canonical synthetic vectors,
  transaction identity and authorization-binding tests.
- Shielded database compatibility guard for stopped-copy migration.
- Independent mandatory shielded proof/codec test registrations and CI checks.

These source changes share the public dinero-main history. Separate
historical branch results do not qualify their combination; this candidate needs
its own full Linux build and test runs.

## Recovery fixture qualification

The full Linux Tests run `36133996292` failed in the offline CSN replay fixture,
which selected default regtest magic instead of the daemon's persisted PoW
profile magic. Strict flatfile framing exposed that mismatch. The fixture now
validates the complete marker and selects its recorded storage identity while
holding the stopped test datadir lock. The storage gate remains unchanged.
Both peer and local-undo recovery pass locally, with malformed/reserved-profile
rejections and a mismatched-network read failure checked explicitly. Fresh
full Linux qualification is required for the corrected head.

## Still required before release

- Connect the typed shared reader to validated mempool, relay, storage and
  block assembly. Typed service descendant connect/disconnect routes now exist,
  but production notification providers and the validated activation-history
  source are missing, so live admission/connection remains disabled.
- Connect the staged coin/signature/spendability components to authenticated
  runtime state; qualify anchors, nullifiers, pool conservation and activation.
- Atomic Orchard frontier/pool/nullifier updates with UTXOs, tip and undo;
  disconnect, restart, reindex, crash recovery and cross-boundary reorg tests.
- Wallet keys/addresses, proof construction, witness maintenance, shield/send/
  unshield and durable interrupted-operation recovery.
- Complete old/new compatibility, platform, loaded-node and combined lifecycle
  qualification, including production binary provenance.

The new pool starts empty under the owner's release-scope decision. Historical
validation, transparent funds and consistent retired-value accounting remain
requirements. Mainnet activation is unset; this branch deploys nothing. Enabling
the staged backend build option does not activate Orchard transactions.

Security-review materials are maintained separately. This source branch carries
implementation and appropriate regression tests, not the entire review archive.

## Delivery status

Most release integration remains unimplemented. Component checks do not reduce
the following rows to test-only work:

| Area | Current state |
| --- | --- |
| Shared parsing and authorization | Typed daemon stored-body queries now route by selected height; exact candidate-bound authorization remains staged; no live admission |
| Mempool, relay, block assembly and acceptance | Typed transparent conflict/staleness reconciliation staged; Orchard admission, nullifier conflicts, relay, mining and production block notifications not implemented |
| Anchors, nullifiers, pool and atomic storage/undo | Ordered mixed coin/fee validation, ChainDB coin/state/undo staging and private-clone forest transitions implemented; durable forest/tip/height staging implemented; body/active transaction indexes and versioned commit record staged together; tip-local audit and process-exit boundaries tested; typed service descendant routes implemented; activation history and production consumer readiness still missing |
| Wallet keys, addresses, proving, shield/send/unshield | Staged ZIP32 keys/receivers, note reception, incremental witnesses and fresh shield/send/unshield proofs; canonical witness persistence and encrypted snapshot storage staged; typed scan/checkpoint restore implemented as a component; origin callbacks, operation job/archival callbacks and live wallet integration not implemented; combined account snapshots, address counters and Reserved/Ready persistence implemented as components |
| Restart, reindex, reorg, crash, platform and loaded-node qualification | Orchard end-to-end qualification not started |

The next state integration must use the authoritative ChainDB write batch for
Orchard state, coins, tip and undo together. An isolated successful proof test
or a separate Orchard database commit cannot satisfy that requirement.

### Authenticated automatic account parent selection (2026-09-26)

Bound connect now persists its actual parent snapshot revision in the encrypted
account payload (DNORAC06); bound disconnect uses that authenticated locator and
restores the preceding parent link. Intervening non-chain revisions do not retarget
undo. Legacy accounts without a link refuse automatic typed undo. See
[account parent links](orchard-account-parent-links-2026-09-26.md).
The prior aea Linux run failed at the new account test's GNU static link order
before root tests; the wallet archive now precedes its chainstate dependency.
This remains a bound consumer component, not installed three-store recovery,
production notification readiness or completion of steps 1–4.

### Retained canonical replay context (2026-09-26)

The existing outbox now retains DNOE03 replay context captured from the actual
sealed indexed write: parent/next Orchard checkpoints, canonical coin undo and
validation-time MTP answers. Disconnect reuses a checked connect-event locator;
old records expose absent context explicitly. Record/page budgets cover the new
payload. See [outbox replay context](orchard-outbox-replay-context-2026-09-26.md).
Immutable intermediate branch views, account/coordinator integration, baseline
reconciliation and production provider installation still remain. This is not
completion of activation history, startup/replay/reindex or user steps 1–4.

### Three-store account recovery (2026-09-26)

The selected service now captures checked source material and builds immutable
branch-specific account restore views. Retained inputs/timing reverify real
Orchard authorizations, and branch anchor/nullifier membership reconstructs the
sealed state transition. `ResumeWalletStores` coordinates existing index,
ordinary-wallet and authenticated account receipts, applying only lagging
stores and retrying partial commits after reopen. Authenticated parent links
drive account undo. The actual adapter is compiled but is not installed in
notification/startup routing. See [coordinated account recovery](wallet-account-coordinated-recovery-2026-09-26.md).

Missing enrollment, old source without replay context, unproven baselines and
the explicit capture limits refuse; late-account/rescan/general long-history
reconciliation and remaining consumer readiness remain required. This does not
complete provider installation, independent activation history, full startup/
replay/reindex or user steps 1–4.

## Stored imported Taproot key resolution (2026-09-27)

The actual script-to-key lookup now reads and validates the existing imported key, mapping, watched script, imported address and encryption owner in one checked database snapshot before HD fallback. Reopen/decrypt-and-sign primitive coverage and refusal of inconsistent records are described in [wallet-taproot-key-lookup-2026-09-27.md](wallet-taproot-key-lookup-2026-09-27.md). Transaction signing/path gating, migration of previously plaintext imports during wallet encryption, complete discovery/reconciliation and selection/provider readiness remain open. Steps 1–4 and the release are not complete.

## Imported transaction signer and builder (2026-09-27)

The existing Taproot transaction signers accept validated imported origins while retaining full key-to-script checks. The builder and sendmany key map use exact outpoint bindings for imports. See [wallet-imported-transaction-signing-2026-09-27.md](wallet-imported-transaction-signing-2026-09-27.md) for synthetic candidate and component-test scope. Actual RPC/broadcast, the distinct sendtoaddress provider, prior-import encryption migration, complete ownership/reconciliation and remaining activation/provider obligations are still open. Steps 1–4 and release qualification remain incomplete.

## Input-bound ordinary and hybrid key providers (2026-09-27)

The separate TransactionSigner now uses exact input bindings with both map and hybrid P2MR providers. Actual sendtoaddress, consolidation and legacy shielding callers supply those bindings. Component tests cover reopened imported keys, retained HD lookup, mixed P2MR signing and required refusals; RPC execution and broadcast remain open. See [wallet-input-key-provider-2026-09-27.md](wallet-input-key-provider-2026-09-27.md). Encryption migration for populated imports, complete ownership/reconciliation, consumer installation and independent activation remain required. Steps 1–4 and release qualification are incomplete.

### Populated wallet encryption transaction (2026-09-27)

The actual encryption and passphrase APIs now transactionally preserve the same
seed, imported private keys and existing P2MR master wrapper across policy
changes. Live state publishes after commit. Ordinary explicit decryption is
supported when no P2MR wrapper exists; that wrapper requires encrypted policy and
otherwise causes refusal. This does not complete encrypted-record discovery,
legacy import registration/lookup, unencrypted P2MR ownership or recovery
readiness. See [wallet-encryption-owner-2026-09-27.md](wallet-encryption-owner-2026-09-27.md).
All five stages remain active; steps 1–4 and release qualification are incomplete.

### Forward private-key import owner (2026-09-27)

The actual private-key API now uses the current Taproot derivation and checked
import owner, preserving other imported addresses. Existing legacy import
inventories refuse this route pending reconciliation. RPC callers bind the wallet
session; backup imports also bind the recorded address. This is forward import,
not complete legacy recovery. See [wallet-private-key-import-2026-09-27.md](wallet-private-key-import-2026-09-27.md).
Steps 1–4 and release qualification remain incomplete.

Local qualification for the forward import batch: fresh actual ON/OFF full daemon
and declared targets passed; ten/eight CTests and fresh all-89-project-C++
ASan/UBSan (34 cases) passed. Three omission controls fail intended assertions;
restored three import cases pass. RPC entrypoints compile but are not executed by
this lane. Historical imports, complete discovery and release gates stay open.

### Known script reload — 2026-09-27

The actual wallet-to-index reload now captures the union of watched and nonempty
address scripts with checked recorded-path consistency under the wallet lease.
It performs no SQL backfill or inferred HD derivation. The index merges the
prepared map atomically and refuses conflicting live paths; the wallet binding
service returns failure on reload refusal. This does not certify key ownership,
authenticated complete discovery, stale-registration reconciliation, delivery
readiness or completion of steps 1–4.

Final script-reload qualification: real ON/OFF full daemon and declared targets,
11/9 CTests, all-89-project-C++ ASan/UBSan with 37 cases and three omission/original
controls passed their required outcomes. Restored three reload cases passed.
The daemon binding caller compiled but was not executed in this component lane.

### Wallet-dependent regression archive links (2026-09-27)

Linux default/QUIC/core-heavy at `660f362b` exposed backward wallet-to-chainstate
and core references in two existing mempool test targets. Linux archive grouping
now retains real implementations and the independent Orchard lane requires both
CTests and all twelve existing cases. See
[scope and qualification](wallet-test-archive-link-2026-09-27.md). Actual Linux
completion remains required before calling the build repair qualified.

### P2MR test-runner portability (2026-09-27)

The new key-read case runner uses explicit function-pointer pair types for GNU
compiler compatibility. Existing test bodies and all three required CI markers
remain unchanged; the failed original Linux build does not qualify the new
component until the corrected source completes. See the
[P2MR key-read scope](wallet-p2mr-key-read-2026-09-27.md).

### Existing master seed envelope reads (2026-09-27)

The actual loader now validates SQL field types, exact envelope size, supported
versions and completed reads before returning an authenticated seed. Current and
legacy KDFs are preserved; malformed or interrupted reads refuse without live
publication. This is a prerequisite for staged unlock, not its completion. See
[scope and qualification](wallet-seed-read-2026-09-27.md).

## Storage test archive linkage (2026-09-27)

The latest full Linux/QUIC/Core Heavy builds exposed pruning/header-status link
failures. Both real storage fixtures now use existing non-mempool isolation and
Linux archive groups, with an independent mandatory execution lane. Fixtures,
production code, assertions and deadlines are unchanged. See
[storage test linkage](storage-test-archive-link-2026-09-27.md); new Linux
qualification remains required before advancing the draft PR.

## Initial mining/coinbase fixture linkage (2026-09-27)

Full Linux builds exposed two further missing daemon-only mempool symbols after
the storage target links passed. Existing non-mempool isolation now covers the
two unchanged height-one fixtures, and the dedicated lane requires their actual
execution. Full Ninja builds collect independent failures while retaining a
nonzero failure exit. See [fixture linkage](height-one-test-link-2026-09-27.md).
New Linux qualification remains required; the draft PR has not advanced.
