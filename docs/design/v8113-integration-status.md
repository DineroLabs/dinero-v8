# v8.1.13 integration status

This branch collects release implementation for review and qualification against
`dinero-main`. It is a draft candidate, not a release or an activation decision.

## Included source

- Pinned Orchard backend, immutable C++ signing context and bounded draft outer
  transaction envelope, with exact-source component and root-build CI jobs.
- Typed shared reader with historical-format regression tests and a default
  legacy-parser rejection boundary for marked Orchard envelopes.
- Transparent mempool reconciliation now accepts typed connected-block effects.
  The mixed-body adapter extracts actual Orchard and ordinary transaction IDs
  and transparent prevouts; the existing historical callback delegates to the
  same eviction/staleness path. This is a prerequisite for production block
  notification, not Orchard admission: nullifier conflicts, wallet events,
  relay and actual ConnectTip/DisconnectTip routing remain open.
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
  Production ConnectTip/DisconnectTip callers are not wired yet.
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
  block assembly; production admission/connection callers still reject Orchard. Selected stored-body queries are now typed, but do not validate or admit blocks.
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
| Anchors, nullifiers, pool and atomic storage/undo | Ordered mixed coin/fee validation, ChainDB coin/state/undo staging and private-clone forest transitions implemented; durable forest/tip/height staging implemented; body/active transaction indexes and versioned commit record staged together; tip-local audit and process-exit boundaries tested; production callers not integrated |
| Wallet keys, addresses, proving, shield/send/unshield | Staged ZIP32 keys/receivers, note reception, incremental witnesses and fresh shield/send/unshield proofs; canonical witness persistence and encrypted snapshot storage staged; typed scan/checkpoint restore implemented as a component; origin callbacks, operation job/archival callbacks and live wallet integration not implemented; combined account snapshots, address counters and Reserved/Ready persistence implemented as components |
| Restart, reindex, reorg, crash, platform and loaded-node qualification | Orchard end-to-end qualification not started |

The next state integration must use the authoritative ChainDB write batch for
Orchard state, coins, tip and undo together. An isolated successful proof test
or a separate Orchard database commit cannot satisfy that requirement.
