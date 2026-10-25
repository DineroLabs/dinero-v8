# Orchard recovered-seed enrollment and mobile recovery

## Scope and release status

The bounded implementation adds `wallet.orchard.enrollpreactivation` and explicit
`wallet.orchard.recoveraccount0` for an unlocked, named recovered wallet. The
latter requires complete authenticated retained history from activation; it is
not yet phone historical download, seamless mobile sync, or release qualification. Public activation
schedules and consensus rules are unchanged. Mainnet Orchard starts at H150000;
implementation uses the selected network parameters, not a hard-coded height.

Both apps must use `wallet.restore` to create a separate named wallet, then
`wallet.open` to reopen it. Check the active wallet and deterministic first
Taproot address. Never replace the generated default wallet's phrase, select a
different seed, or weaken the seed-orphan guard. Same-seed mnemonic import remains
idempotent, but is not the first-bind mechanism. On refused queue requests the
apps must clear the corresponding unaccepted local request ID, without dropping
requests that Core actually committed. No app changes are made by this Core work.

## Implemented bounded enrollment contract

The public bound RPC takes no birthday, height, checkpoint, network or account
argument. The selected service obtains the profile and checkpoint. It requires:

- Service and exact wallet/session lifetime ownership, no borrowed selected lock
  or SQLite owner, and a supported scheduled Orchard profile.
- A converged selected tip, identical durable and validated tip, authenticated
  accumulator backing, no initial download or incomplete assumed-state history.
  Public-network tips older than two hours refuse; regtest permits offline time.
- A tip strictly below activation and no retained activation/outbox history,
  including after a reorg back below activation.
- An authenticated recovered initial seed owner and intact unknown catalog;
  no missing/corrupt catalog repair and no replacement of generated provenance.
- No current, retained, archived or pending Orchard owner rows. Partial or
  malformed wallet storage refuses. No new schema is manufactured to pass.

Snapshot/load and selected locks serialize the source check through a FULL
SQLite transaction. Seed and wallet identity remain pinned until commit. An
existing matching empty enrollment is idempotent; it does not refresh or reset
the certificate or account state. Required-write/commit failure publishes none
of the new provenance. No callback or network request occurs under wallet SQL.

Existing `DNOAC01` generated/unknown records keep their encoding. Enrolled
recovered wallets use `DNOAC02`, with `generated=false`, a separate provenance
mode, and a seed-authenticated wallet/profile/checkpoint record. Its authority
is limited to the recorded network, genesis, branch and activation rules.
Initial PQ ownership is unchanged. Every existing inventory, retained revision,
reservation and orphan-state check still applies. Unknown recovered wallets do
not enroll implicitly on account creation, balance reads, signing or startup.
Older Core versions do not understand the new catalog mode and must fail closed;
enrollment is not a promise of wallet downgrade compatibility. Preserve backups.

The checkpoint is an as-of observation, not proof of a globally latest chain.
Header convergence and tip age are conservative liveness policies, not protection
against every eclipse or hidden competing chain. The consensus fact certified
is empty Orchard history through a pre-activation checkpoint. It is not a
certificate of current balance or completed post-activation scanning. A later
pre-activation fork cannot contain Orchard notes under the same bound rules;
actual account state still starts at the authenticated activation parent of the
selected replay source and follows normal connect/disconnect verification.

## Retained-history post-activation account-0 recovery

This is required for new installs, reinstalls and new phones after activation.
Core cannot distinguish an app-generated phrase from an old recovered phrase.
Neither case may get a trusted-empty current wallet just because it uses
`wallet.restore` or supplies a recent birthday.

The Core RPC implements the following bounded contract when its selected service
can supply the complete activation-origin replay. It does not make missing
historical data appear on a snapshot-only/stateless node.

1. Explicit account-0 recovery reports coverage as account 0,
   not every possible BIP32 account of the seed. Arbitrary/nonconsecutive Core
   accounts require an explicit bounded account set or authenticated catalog
   backup. Do not silently erase or declare recovery of unlisted accounts.
   Account-scope expansion requires the corresponding history scan.
2. Capture selected network/genesis/branch/activation, a validated target hash
   and height, and the actual activation-parent hash. The empty tree at that
   parent is the starting baseline, not the final recovery result.
3. Scan every selected block from activation through that target, gap-free.
   Trial-decrypt both wallet scopes locally, process all note commitments in
   exact order, track owned nullifiers/spends and witnesses, and retain receipt
   txids, action positions, heights, hashes, amounts and memos. Compare computed
   tree sizes/roots with authenticated chain commitments. Reorged ancestry or
   any missing body/proof/checkpoint refuses completion.
4. Use a resumable encrypted disk-backed staging owner separate from the active catalog.
   Persist verified prefixes and their identity, not a caller-chosen cursor.
   Derive only the necessary viewing authority under a short wallet lease;
   download/decrypt/verify outside wallet/selected locks. Encrypt sensitive
   temporary material and keep seeds/viewing keys off remote services.
5. Before publication, recheck wallet/session, account scope, existing DB
   ownership and selected ancestry. Commit the recovered account snapshots,
   scoped catalog provenance and scan checkpoint atomically with FULL durability.
   A moved target means an explicit retry/rebase or a clearly labelled verified
   prefix, never `caught_up=true`. Signing and spend admission retain fresh
   selected-source checks. Process death must never expose partial recovery.
6. A phrase recovers on-chain notes, not lost off-chain payment requests,
   reservations, previously issued unused addresses or cross-device exclusivity.
   Existing retained owners must be reconciled/preserved, never overwritten by
   an empty scan. No automatic rebroadcast or reservation release follows from
   a recovery certificate.

Each call processes at most 64 delivery events after the selected source has
been captured and verified. `complete=false` is a verified staged prefix, never
an active wallet owner. Repeated calls resume after encrypted wallet reopen.
`restart=true` discards only uncertified recovery cache after rechecking the
intact unknown/empty active catalog. It never repairs missing/corrupt catalogs,
replaces a seed, or resets an already admitted owner. Generated provenance stays
false. `DNOAC03` binds the seed-authenticated wallet, network, activation parent,
account 0, scanned target and delivery digest. Other accounts remain refused.

Publication atomically adopts the verified encrypted staging tables with the
catalog in one FULL transaction, after fresh selected-tip/source/session checks.
Staging is trimmed after every bounded call to at most 256 immediate parent
snapshots; the next 64-event chunk may temporarily add at most 64 more. The
initial admitted owner keeps that same window. A deeper source/wallet rewind
fails closed and requires a complete rescan source; that deep-reorg rescan path
is still open, not silently synthesized. A retained outbox containing a deeper
rewind is not necessarily repairable by repeating `restart=true`. Existing inventory and pending-owner checks are
unchanged. Recovery cannot reconstruct lost off-chain requests or unused receiver
issuance history. A complete certificate is an as-of scan, not perpetual freshness.

Runtime replay capture now streams retained origin material into private disk
storage without the former 2048-record/64MiB total-history ceilings. Branch
metadata and cursor positions are disk indexed under a verifier-owned
content-addressed root; original authorization/state token digests remain bound
independently of their scratch-record storage MACs. Event/body/state/authorization
caches and SQLite pagers remain bounded; source-page, per-record, source-head,
ancestry, real-proof and EOF checks still apply. Disk/allocation errors refuse;
there is no truncated-prefix success. This is not a total RSS, latency or disk
budget certificate and does not supply missing historical material.

These private source scratch spools cannot reopen after process death. The
separate encrypted wallet staging owner can resume, but the source is rebuilt
and reverified. Source-capture latency, disk use and device feasibility remain
unqualified. Authenticated historical download and real-device qualification are
still required for phones; replay scalability alone grants no mobile authority.

## Stateless phone: seamless transfer selected, integration unfinished

The owner selected automatic authenticated catalog transfer, not a manual v6
file or a fresh-datadir reset. The new boundary wire codec pins genesis, network
magic, branch, activation, maturity-leaf rule, selected activation parent/block,
and a local request nonce. A bounded sequential 64-KiB page stream uses private
MAC-checked disk staging and a complete-content digest. Completion reuses the
existing DNORST02 verifier and configured burial depth; peer hashes and headers
alone cannot enroll a compact owner. The first activation proof carries complete
parent/base catalog reachability and the full activation body, not path-only
membership or caller metadata. No new consensus schedule is introduced.

Archival proof preparation now reconstructs that first block after the full-node
tip has advanced: retained parent/base catalogs and the complete first body are
read into a bounded private disk inventory; genuine coin/Orchard authorizations
derive the first frontier and membership sets from the empty activation origin.
The locally selected buried DNORST02 commitment authenticates the result. Current
sets are not rewound/copied, and the source database is not changed. Cancellation
is checked per source-node read and between proof phases, not during an individual
cryptographic operation. This preparation API is not a live owner handoff or
mobile historical source.

Full, non-pruned nodes now advertise a separate catalog-serving capability and
route `getocat`/`getocatpg` through a single bounded worker, outside peer/selected
locks. Preparation takes a real service shutdown-use; the source/header owners
are checked again after proof work. One immutable MAC-checked disk payload is
shared by up to eight connection-bound nonce sessions, with at most 32 queued
requests and one queued flight per connection. Replies retain the requesting
connection nonce; a replacement connection cannot be selected by a delayed send.
Malformed, foreign, unburied or unselected requests cannot start preparation.
Failed preparation has a global one-minute cooldown. Preparation cancellation
is cooperative at the documented checkpoints; Stop joins the worker before
destroying its source or P2P manager.

Per-peer/global serving rates are 16/64 pages per second, with an eight-GiB
hourly global wire budget, 30-second idle/queue expiry and a 30-minute session
lifetime. A disconnected/replaced peer cannot receive a session-bound reply;
its idle session is discarded within the expiry window on the next work item.
The compact snapshot load/export switch does not disable catalog validation or
serving. These are local resource policies, not changes to consensus limits.
Client scheduling, daemon handoff and historical recovery downloads are still
separate unfinished integration work; serving capability is not phone readiness.

A private activation-origin verifier now starts at the authenticated parent,
not at the first block's already-computed snapshot state. It verifies the full
first block and every gap-free selected-header descendant, including scripts,
Orchard proofs/anchors/nullifiers, pool bounds, filter, stump and DNORST02 root.
Nullifiers, anchor occurrences and catalog deltas use private MAC-checked disk
storage; one block result is returned at a time, with no all-history body/token
vector or duplicate historical header selector. The immutable first catalog is
shared with a mutable delta overlay, not recopied per block. Grouped ordered
anchor occurrences derive exact reference counts without mutable spool records.
Each failure poisons the verifier; incomplete targets never finish. Metadata is
copied before enrollment so external mutable aliases cannot rewrite its origin.
This is proof/state-token construction, not canonical admission or a certified
wallet source. Client wiring, runtime-delivery cursor binding and fresh wallet
adoption remain mandatory. Disk/RSS/throughput/device budgets are not qualified;
the consensus-required set digests still scan the accumulated membership rows.

This transport is NOT yet client-driven atomic transition of an
existing H149999 store, crash-safe startup recovery, or a wallet-history source.
Those integration gates remain open. Automatic crossing may wait at the
activation parent for the configured commitment burial. It does not require a
user-managed snapshot, but does require an available archival peer and complete
local authentication before crossing. Never lower burial or disable catalog
validation to call crossing immediate or seamless.

Proposed initial transport is on-demand full canonical block download from
archival peers, not a trusted balance service. A phone need not keep history
permanently: spool bounded verified data, update the note tree/owned witnesses,
persist the verified prefix, then discard temporary bodies according to the
reorg/crash policy. An HTTP gateway may transport the same bytes, but cannot
choose a scan start, omit blocks/transactions or supply authoritative balances.

Authenticate complete bodies/transaction counts and Merkle roots against the
locally validated selected chain, enforce Orchard encoding/profile and state
commitments, and reject gaps, duplicates, reordering and fork substitutions.
Header PoW or a server's tip assertion alone is NOT full chain validation.
Use the existing verified compact-state/proof owner for chain validation; its
startup/boundary and proof-retention paths must be qualified independently.

Ordinary transparent compact filters do not prove a complete Orchard scan. A
future Orchard-specific compact protocol would need authenticated complete
commitments, ciphertexts, nullifiers, positions and omission protection for
every covered block. It is a separate protocol, not a shortcut assumed here.

A v6 snapshot authenticates network state; it does not contain a recovered
seed's complete historical notes/memos or prove account discovery. Availability
of an archival provider remains necessary for this proposed scan. Missing
providers leave recovery incomplete, with a useful error and no spend readiness.

The reported stateless H149999 stop is an independent release blocker. Enrollment
does not fix it. The current timed soak has three full producers and an empty
Orchard tree; a separate completed short snapshot matrix covers both profiles.
Neither proves an ordinary mobile install can cross the boundary. Diagnose and
fix the actual historical-to-Orchard compact owner/validation path in a separate
reviewed commit; test both direct boundary crossing and v6 bootstrap. Do not
disable catalog/root validation, silently switch to trusting a server, or infer
physical-phone viability from host/synthetic memory measurements.

## Acceptance gates

The owner selected automatic internal close/reopen, not an in-place raw-forest
swap. Before it is enabled, it must preserve/reopen the active named wallet
without importing its phrase; suppress semantic queueshield/queuesend refusals
during the pause (retryable restarting/no reply); retain/revalidate or re-broadcast
mempool and durable pending private payments; and pass a real activation-crossing
regtest with a named wallet and queued payment. These gates are not yet cleared.

Wallet and mempool services now offer reversible, thread-affine admission fences.
Existing operations drain; abort resumes the same manager/pool; commit permits
ordinary Stop with admission still closed. An inactive/foreign fence cannot read
state and same-thread Stop requires an explicit fence decision. Existing wallet
operations may finish lazy proof-worker setup during this reversible drain, but
not during actual Stop. Native tests cover named-wallet fence abort/cold open
and export/normal re-admission of genuinely accepted mixed-family candidates.
No durable candidate journal or automatic restart is enabled by these fences
alone; their callers must fence RPC first and persist work before commit.

HTTP and the embedded RPC bridge now check a shared monotonic reopen epoch
before and after dispatch. HTTP returns retryable -28/restarting without a result;
the embedded bridge returns no reply (NULL), so callers retry the same request ID.
An entire pause/abort inside a call still invalidates that call's result, including
a transient queue refusal. A pause must start before service fences, and survive
until the same owners resume or replacement services are fully queryable. NodeCore
retains the gate independently of the App/context. These transport checks are
tested with real queue names behind closed wallet/pool admission, and with a
deterministic in-handler epoch change. They are not an automatic restart or a
live owner install. Other transports must be stopped/drained before service
fences (or bound to the same epoch); an HTTP/FFI pass alone does not qualify them.

The reopen candidate journal is a separate prerequisite, not startup wiring.
It captures exact public transaction and cached-proof bytes while the genuine
pool fence and operator-root lock are held. No fee, accepted flag, ownership,
proof-freshness or caller-supplied validity becomes restoration authority. It
streams at most100,000 rows/1GiB, with a32MiB per-wire bound and a512MiB free-disk
floor. The root and held lock inode, profile (including wire magic), private
directory, no-follow regular-file/hardlink constraints and complete inventory
digest are checked. Sealing syncs the file and directories and uses no-replace
publication; partial/ambiguous files are retained and refuse. Opening a sealed
journal re-establishes durability, then a full audit precedes each visitor.
Normal canonical admission/relay must revalidate every restored candidate;
unresolved dependencies or stale proofs remain retained, not silently discarded.
There is no automatic consumption/deletion or direct pool insertion API.

Native fixtures cover actual mixed-family candidates, cached-proof retention,
closing/reopening the journal and lock, canonical readmission, aborted capture,
and corrupt/profile/alias/hardlink/root-replacement refusals before visitor use.
This is not a real cold-daemon activation crossing, power-loss matrix, mobile
filesystem/RSS qualification or automatic mempool/pending-payment restoration.
Windows journal capture remains explicitly unavailable/fail-closed. Private
wallet intents stay in their existing encrypted durable queue; no mnemonic,
password, randomized proving plan or unlock continuation is written here.

The App now has an explicit orderly-reopen receipt API. It pauses RPC, fences
wallet then pool, captures the current named wallet and durably seals the genuine
current pool before committing closed admission. Only that private path skips
the legacy best-effort save, which otherwise cannot borrow a closed pool. Normal
Stop/save behavior is unchanged. Full Stop still joins old services/readers;
the host must destroy that App before replacement Init under the same root lock.
The receipt is not state-conversion or catalog authority. It rechecks its root
before Init and resume; the replacement must be a fresh started App on the same
RPC epoch. Init, including failed Init cleanup, preserves the inherited gate.
Resume uses normal named-wallet open without a phrase/unlock bypass and normal
canonical proof/body admission with a distinct core-reopen origin. Unresolved
candidates remain retained. Successful resume alone releases RPC; abandoning an
irreversibly committed receipt stays paused/fail-closed. An SDK caller may retain
a shared pause across its publication-mutex/App-move gap so an aborted capture
does not unpause before the SAME App is republished.

The real linked CoreOrderlyReopen fixture closes and destroys one App, starts a
fresh App with that receipt, checks paused queue calls return retryable -28, and
opens the named wallet without its phrase. It is a sequential native host fixture
with an empty pool at genesis: NOT the automatic NodeCore worker, activation,
nonempty private queue/mempool, authenticated catalog conversion or phone-history
acceptance. The required real queued-payment activation crossing remains open.

An orderly close now durably selects exactly one current journal and named
wallet before committing Stop. Only the private fresh-capture path may select
this lifecycle intent; opening old candidate files grants no such permission.
The bounded control record binds the journal's profile/root inode/nonce and
full-corpus footer digest, the wallet name, and its own checksum. No seed,
password, private payment plan or chainstate authority is added. Root/lock
ownership, no-follow/regular-file/owner/mode/hardlink checks, full journal audit,
synced writes and no-replace publication remain mandatory. RPC is irreversibly
latched closed before intent publication starts. Partial or ambiguous control
names refuse; no automatic cleanup or overwrite is permitted. Unselected sealed
captures remain diagnostic evidence, not automatic rebroadcast permission.

Pending read opens only that exact journal; it never scans for a newest capture.
After all candidates resolve through normal canonical admission, completion
archives the control by no-replace publication and keeps the original corpus.
Unresolved candidates retain the pending selection and block another close from
overwriting it. The App receipt checks the intent before replacement Init and
resume. This adds durable identification, not a completed cold-process/SDK
recovery worker or a power-loss qualification. Those paths must recover only a
valid selected intent and keep RPC closed until the named wallet and canonical
readmission are ready; they may not adopt a live pause token or reset an epoch.

Owner decision: automatic crossing must retain H149999 (activation minus one)
as an available compact undo/cold-reopen state. It may not silently establish
H150000 as the snapshot/reorg floor. The activation-parent proof, original
before-images, genuine first-block validation, canonical undo and runtime event
must therefore survive first connect, disconnect, cold reopen and reconnect.
This remains separate unfinished owner-install/startup work; none of the
generic reopen APIs or transfer codec grants that state-conversion authority.
The running 72-hour private soak is three full producers on an older pinned
source with an empty Orchard tree. The earlier short six-consumer matrix covered
both v6 profiles; neither is a 72-hour compact/nonempty/new-source qualification.

The thread-affine catalog download actor now handles one capability/connection/
nonce-bound flight, sequential disk-backed pages, a single identical page retry,
16 requests/second, an 11-minute initial manifest wait (server preparation is
bounded to 10 minutes), a 30-second page idle and 45-minute total flight, cancellation
and a conservative free-disk check before staging. It authenticates completion
with the existing full DNORST02 verifier and yields only an immutable unpublished
section. The host must serialize calls on its owner worker, use a real steady
clock, enforce cross-attempt cooldown/peer rotation with fresh nonces, and drain
that worker before teardown. Foreign nonce/transfer-ID late replies on the same
connection do not acknowledge or extend a flight. Matching-flight corruption
still fails. Verification itself is not asynchronously interruptible.
This actor is tested against real generated proof pages, but is not yet wired
to daemon peer scheduling, live-owner installation or phone history downloads.

SDK store ownership now acquires the daemon's real datadir guard before Init,
pins the resolved operator root through all arguments and DaemonApp, and holds
the guard until the worker, services and database have closed. The persistent
maintenance fence is checked both before lock acquisition and on that pinned
root. Native executable tests exercise root-lock contention through alias and
resolved paths and release after completed stop. This prerequisite does not
itself install a catalog or qualify iOS/Android filesystem behavior.

The SDK now opts into the existing reviewed separated-layout upgrade under that
guard, including stateless profiles. A brand-new scheduled store first runs an
offline genesis bootstrap under the same lock, persists its real cohort, closes
all stores, then performs the ordinary copy/verify/swap before the real Init.
Existing stores skip the fresh bootstrap. The migration engine, original LOCK,
checksums, synced writes, cohort checks, no-follow/inode/hardlink rules, space
budget and interrupted-swap handling are unchanged. Refusal still keeps legacy
storage and reports inability to follow Orchard; it grants no compact owner.
Native tests cover fresh and existing SDK stores, seed-row preservation, cold
reopen and the inner-symlink refusal. The shared lock uses real flock on iOS too;
only unavailable libproc process-name diagnostics use the pre-existing
conservative live-PID fallback. This is not a framework or device qualification.

- Existing unknown/missing/corrupt/untracked catalog and seed-replacement
  refusals remain negative. New enrollment tests cover atomicity, source gaps,
  borrowed owners, reopen and actual funds received across activation.
- Post-activation positive: a fresh named wallet restored after activation finds
  funds shielded earlier to account 0, including memo and spent-note handling;
  matches the original wallet, can spend, and survives two cold restarts.
- Android `OrchardWalletLiveRegtestTest.appRecoveryPhraseBinding_*` is an
  acceptance check, not currently satisfied by the enrollment-only commit.
  Run an equivalent iOS app-generated-phrase path, not a Core-generated wallet.
- Missing/tampered/forked/omitted scan data, wrong profile/seed/session, concurrent
  source movement and interrupted publication must fail closed without changing
  existing owners. Qualify new install, reinstall and second-device recovery.
- Demonstrate real stateless boundary sync and authenticated historical recovery
  on physical iOS and Android devices with measured disk, RSS/footprint,
  background suspension and interrupted restart behavior. No device authority
  or device success is implied by this plan.

If only enrollment is shipped, release notes must explicitly exclude
post-activation new-install/phrase-only recovery. Even pre-enrolled private
payments cannot be advertised on mobile until the H149999 sync blocker and
actual app/device gates pass. The November 2 go/no-go remains independent.
