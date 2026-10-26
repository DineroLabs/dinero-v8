# Orchard recovered-seed enrollment and mobile recovery

## Scope and release status

The bounded implementation adds `wallet.orchard.enrollpreactivation` and explicit
`wallet.orchard.recoveraccount0` for an unlocked, named recovered wallet. The
latter requires complete authenticated retained history from activation; it is
not yet complete phone historical recovery, seamless mobile sync, or release qualification. Public activation
schedules and consensus rules are unchanged. Mainnet Orchard starts at H150000;
implementation uses the selected network parameters, not a hard-coded height.

The late compact-v6 source now has an explicit requested full-body download
stage. For an intact bound unknown recovered wallet, `recoveraccount0` can
request the buried activation catalog and sequential `MSG_BLOCK` bodies from
activation through the locally selected tip. Each body is genuinely checked
from the authenticated activation parent (including the first block); the
terminal catalog must match the selected state. One session-bound flight,
bounded ingress, timeout/cooldown, per-minute wire budget (including invalid
bodies), disk reserve and private disk staging limit acquisition resources.
Targets may extend only along the selected descendant branch; changed branches
discard the unadopted source, never active ownership.

This stage returns `complete=false`, `applied_sequence=0` and transport-only
`history_*` progress. `history_ready` means DATA ready for a separate authenticated
wallet scan/adoption, not recovered ownership, usable funds or a canonical
delivery cursor. The snapshot-floor outbox remains unchanged. No catalog is
initialized/repaired and no runtime event is fabricated for downloaded history.
Fresh late-phone wallet adoption, cold-source retention and nonempty queued
activation crossing remain open; this stage must not be marketed as those gates.

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

Completed activation-handoff startup has a distinct selected-state consistency
capability; it does not advance the original independent validated anchor. Its
generation/mutex/database/profile/origin/anchor checks now also guard selected
accumulator observations, delivery-history reads and compact pool/ingress context.
The ordinary full-store and independently replayed compact paths retain their
validated-tip equality requirement. Exact catalog/stump/header/work/storage and
body/proof checks remain mandatory; a matching origin row alone is not authority.

The native `OrchardActivationRuntime` test cold-starts the real App twice on an
authenticated descendant, resubmits its identical canonical body, obtains the
public binding for its named wallet and invokes explicit account-0 recovery over
retained local history. It preserves the original anchor and generated=false.
This fixture does not establish phone history downloads, funds belonging to that
phrase, queued-payment crossing or an original mobile assumed-history lifecycle.

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
The stateless daemon now schedules a separate, packet-woken download worker
when its selected legacy tip reaches activation minus one. It waits for locally
selected buried activation headers and a capability-negotiated archival peer.
Peer endpoint/session/capabilities are copied together under the peer mutex;
outbound sends pin that exact connection. The thread-affine client, receive
queue (four packets maximum), private disk decoding and verification all stay
on that worker, outside peer/ingress locks. A 50-ms timer drives quotas and
deadlines independently of the general five-second scheduler. Failed addresses
receive a one-minute cooldown (at most 32 retained entries); new flights use
fresh OS-random nonces. Network disable, replaced/disconnected peers and lost
selection cancel/discard the applicable flight. Stop joins before dependencies
are released. Cancellation is checked while decoding and around complete proof
authentication, not within an individual verifier operation. There is no new
claim of an exact cryptographic cancellation latency.

The completed immutable proof is only unpublished DATA. Its accessor checks
local header selection again; the future closed-store converter must separately
recheck all before-images and authenticate it. No conversion, marker, catalog
binding, runtime event, wallet operation or validation flag is published by the
worker. Daemon handoff and historical recovery downloads remain unfinished;
serving/client component tests are not actual mobile crossing or phone readiness.

### Embedded close/reopen host

NodeCore now schedules its internal activation handoff on the existing node
worker. The worker owns argument strings and thread-affine lifecycle receipts;
the same real datadir guard remains held until the worker has stopped and all
old/new services are destroyed. It detaches and revalidates a coherent
genesis-to-burial header ancestry without retaining an old HeaderStore/forest
reader. Proof data and a genuine drained current-pool/named-wallet receipt feed
the existing closed-store converter and selection guards, followed by normal
completed-root startup. The first Orchard block is not skipped or made a floor.

Long wallet/pool readmission stays behind the held RPC gate. A separate short
publication callback puts the started replacement App in the SDK and marks it
running before opening that gate. Full journal scans/archive happen afterwards,
not under the SDK publication mutex. An interrupted prepare/publication/Stop
retains the selected corpus; unresolved bodies are not archived and completed
journals are retained. Explicit startup recovers a genuine pending named-wallet
intent on its owning worker. A fresh unused recovery shell is destroyed before
closed filesystem recovery; binding the replacement re-applies the unchanged
closed-receipt/readers/root checks before Init. No arbitrary capture is adopted.

Public Stop declares a monotonic cancellation generation before waiting for
the SDK mutex. The internal worker uses only try-lock publication while Stop
may hold that mutex and join it. Failed/uncertain capture stays closed/no-reply;
it is not permission to reset the gate or automatically retry an uncertain swap.
Init keeps its previous unbounded phase and App Start its 30-second confirmation
window; journal/readmission is separate bounded-corpus work. No total latency,
resident-memory or cryptographic cancellation deadline is claimed.

`NodeCoreSdkPendingReopen` exercises actual C ABI cold recovery of the named
wallet plus publication refusal/throw/interruption controls at genesis with an
empty pool. It is not an actual P2P activation transfer, original assumed SQL
lifecycle, nonempty queued payment, phone history or device acceptance. Those
must pass separately, including activation-minus-one undo/cold startup.

Closed-store conversion preparation now creates a separate, private candidate
under the same real resolved datadir lock. The source is opened read-only with
the normal directory reservation, actual ChainDB LOCK, schema/layout and READY
checks; source INFO logging is suppressed rather than rotating its files. A
live source, inner symlink, foreign hardlink or substituted root refuses. Only
an existing SST's exact retained layout-upgrade rollback counterpart may share
its inode. No hardlinks are created for the new candidate.

All original column-family rows are copied with checksums and bounded synced
batches, then independently compared before conversion. Complete selected/buried
DNORST02 proof, actual parent header/work/stump, full frozen legacy before-image,
empty Orchard namespace and the original validated anchor's selected ancestry
are mandatory. This is distinct from the original independently prepared legacy
retirement guard, which continues to require the validated marker at the parent.
The conversion does not advance that marker or alter lifecycle/companion files.

Only the unselected candidate drops its fully parsed coin rows and adds exact
proof/catalog nodes and a compact compatibility binding. A domain-bound origin
record and its selection fence are written in one synced candidate batch. They
bind the original full logical row digest, validated anchor, selected first/parent,
parent catalog, proof and retirement. Missing/conflicting origin or selection
never grants fresh parent authority. Every original non-coin row remains exactly
equal; additional rows are restricted to the fully checked proof/catalog/origin
data. Both stores remain exclusively locked during the final source/target
inventory, physical-identity and selected-header recheck. Cancellation leaves
only an unselected candidate, never an implicitly adopted directory.

This private preparation result is NOT promotion, typed live-state consistency,
parent/descendant service startup or SDK automatic reopen. It retains candidates
for investigation instead of deleting/adopting them. Live selection still needs
a durable closed-store cohort journal and full restart recovery under the same
guard; re-enrollment must reauthenticate and require the selected origin through
activation-minus-one undo. Actual lifecycle/queued-payment/device acceptance and
production-scale preparation disk/RSS/latency remain open.

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
does not fix it. The historical timed soak had three full producers and an empty
Orchard tree; its controller stopped after about 20h40m on an SSH sampling error,
not a 72-hour pass. The owner has deferred further soak work. A separate
completed short snapshot matrix covered both profiles on its older source.
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

The explicit fresh-App `PreparePendingReopen` host API now opens that selected
intent before Init, binds its receipt to this replacement instance, and retains
the same locked root/RPC gate. It does not enroll a catalog or state owner. It
requires a never-initialized App; ordinary active or stopped-old Apps cannot use
it to skip teardown. Missing intent returns null without changing RPC; malformed
or ambiguous controls refuse. The host still must destroy every old service and
reader before replacement initialization. This API is not yet invoked by an
automatic NodeCore worker or general SDK startup.

The RPC epoch now distinguishes OPEN, HELD and abandoned UNOWNED states. Every
state transition is monotonic: no reset/ABA, recovery of a live HELD token, or
generation wrap is permitted. Dropping an irreversible receipt changes HELD to
UNOWNED without admitting RPC. Only the intent-checked host path may acquire a
replacement token, which also remains closed on abandonment. Normal abort still
resumes the same owners. A real linked native host fixture exercises both an
abandoned same-process receipt and complete original driver exit followed by a
fresh driver reopening the named wallet without a phrase. These remain genesis/
empty-pool fixtures, not activation, nonempty pending payments or device tests.

The App host can now prepare normal named-wallet binding and canonical
proof/body readmission separately while keeping RPC closed and the complete
selected durable candidate intent untouched. The receipt is bound to that
replacement App, not transferable to another live instance. An abandoned
prepared replacement may stop and exit; fresh intent-checked recovery retains
the wallet name and public corpus. Preparation never archives the corpus,
opens the gate, converts a store or caches transaction validity. Direct-App
completion still redoes current-owner binding and normal admission before its
existing archive/resume. This is not a short SDK publication primitive: the
automatic worker still needs checked publication/publicStop ordering, and
must not open the gate while its replacement App is unpublished. Native
genesis/empty-pool tests cover prepare/completion and complete host exit/fresh
host recovery, not real queued payments across activation or SDK concurrency.

Owner decision: automatic crossing must retain H149999 (activation minus one)
as an available compact undo/cold-reopen state. It may not silently establish
H150000 as the snapshot/reorg floor. The activation-parent proof, original
before-images, genuine first-block validation, canonical undo and runtime event
must therefore survive first connect, disconnect, cold reopen and reconnect.
This remains separate unfinished owner-install/startup work; none of the
generic reopen APIs or transfer codec grants that state-conversion authority.
The private store now has a separate read-only activation-parent re-enrollment
factory: it authenticates a selected/buried joint-activation transfer proof,
requires an already compact/quiescent store at activation minus one, checks its
exact retained catalog and every reachable parent node, stump/header/work/forest
marker, complete frozen legacy before-image and empty active Orchard namespace,
and rechecks header selection/burial across private enrollment. Full-storage
guards and the original fresh-only v6 importer are unchanged. It installs no
snapshot base, fake empty Orchard state, proof chunks, tip or validation marker,
does not convert a full/assumed store or publish graph/readers, and cannot restore
a descendant through the parent route. The first connect still uses the normal
writer's authorizations, root checks, actual nullifier owners, undo and outbox;
inverse removes the first active state and permits parent re-enrollment after a
database close/open. The host still must retain/authenticate proof data for cold
startup and implement atomic closed-store conversion/activation routing. This
factory alone is not the seamless protocol or a live App crossing qualification.

A separate private activation-proof data cache now retains canonical proof
bytes in bounded immutable, synced ChainDB chunks before atomically publishing
an immutable descriptor keyed by the exact first-block hash. It has no mutable
"latest proof" selector. The descriptor pins wire magic and the consensus
profile, exact first/parent hashes, size and full canonical-stream SHA256.
Reads decode into disk-backed node storage and rerun complete selected/buried
DNORST02 authentication; the digest and local descriptor alone never authorize
state. A quiescent host can discard its original proof object, close/open the
database and recover the data without manufacturing a snapshot base or skipping
the first block. Retaining it does not convert full/assumed storage, publish a
compact owner, change lifecycle/history validation flags, or start services.
Cancelled unselected prefixes may resume only with identical bytes; completed
missing/corrupt/conflicting chunks and wrong profile descriptors refuse without
repair. Root ownership and writer lifetime remain the caller's responsibility.
The per-proof format ceiling and free-space floor do not qualify repeated
branch-cache quotas, crash/power-loss behavior, large-state memory or device
performance. Host scheduling, durable closed-store conversion, startup routes
and automatic SDK/P2P/history integration remain unfinished.

The historical attempted 72-hour private soak used three full producers on an
older pinned source with an empty Orchard tree. Its controller stopped after
about 20 hours 40 minutes due to an SSH sampling failure; it is not a 72-hour
pass. The earlier short six-consumer matrix covered both v6 profiles separately.
Neither qualifies compact/nonempty/current-source operation. The owner has
prioritized integration and acceptance; no new soak is being started here.

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
The actor and bounded packet-woken daemon client host are now wired at component
scope, including connection-bound capabilities/sends, peer rotation and Stop
cleanup. The generated-page roundtrip uses in-process transport; it does not
prove real network activation crossing, live-owner installation or complete
phone history downloads.

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

## Closed-store authenticated activation handoff

Preparation makes an exclusive unpublished READY candidate under the same real
datadir guard, keeping actual source/candidate DB locks. It copies all column
families in bounded checksummed, synced batches, verifies complete ordered row
digests, retains/rechecks the selected buried activation proof, and removes coins
only from that candidate. Full legacy before-images and the original validated
marker/history obligation remain unchanged. Origin/selection metadata is DATA,
not proof that assumed prehistory was independently executed or a compact owner.

Durable selection additionally requires a checked revocable borrow of a genuine
Core lifecycle receipt after all old App readers have been destroyed. The borrow
expires with the receipt and refuses foreign threads, resumed phases and live
replacement Apps. A read-only storage library cannot fabricate this borrow.
The immutable selection control binds the exact root/directories, source and
candidate physical inventories/logical digests, origin/proof/profile, nonce,
selected public-candidate journal and named wallet. Kernel no-replace renames
retain the original store at its exact recorded backup name before selecting
the candidate at the canonical name. Every phase is synced and re-audited; a
separate immutable completion record binds that exact intent. Recovery follows
only the explicit intent and exact directory shapes, never a prefix scan or the
newest abandoned candidate. Broken symlinks, extra links, corrupt/foreign
controls, ambiguous partial publications, changed stores or moved selection
refuse without deleting evidence or guessing a repair. Cancel after durable
selection keeps RPC paused and both stores retained for checked recovery.

These mechanisms are private closed-store components. Generic startup refuses
selected/partial handoff controls before opening/repairing/creating a ChainDB;
dedicated authenticated parent/descendant startup and typed selected-state
consistency still need integration. Selection does not change validated markers,
publish a service, grant a writer or open RPC. Component tests combine real
EMPTY-GENESIS named-wallet lifecycle receipts with a separate offline authenticated
activation-parent fixture. They are not the real activation with queued payment,
whole assumed-state lifecycle, SDK automatic worker, complete phone recovery,
physical device or crash/power-loss acceptance gates.

The handoff candidate now also retains the exact minimal genesis-to-configured-
burial header closure. Header-first sync has a separate HeaderStore, so a source
ChainDB need not already contain ahead-of-body headers (or every pre-base raw
header). The converter reads a captured hash-linked ancestry in 256-row batches
into a bounded-pager, private MAC disk spool in the app workspace, then runs a
fresh HeaderChainSelector forward from compiled genesis and checks every exact
height/work/header encoding. Only missing raw header rows are added; conflicting
original rows refuse. No active height index, body/undo locator or CHAIN/SCRIPTS
metadata is manufactured. A synced immutable descriptor pins profile, magic,
first block and exact configured-burial target. Checksums stay on, source rows
stay unchanged, and the scratch/write batches periodically retain a free-space
floor. This is not a production-scale CPU/RSS/disk measurement.
Header values stream once from the checksummed CF into a bounded-batch private
MAC cache; ancestry reads do not repeatedly materialize a giant cached SST.
Cache absence never authorizes a write: missing-row additions recheck the actual
exclusive DB, and the final whole-store before-image audit remains mandatory.

Cold recovery may reconstruct a fresh selector from those retained bytes when
the old in-memory selector is gone, then reauthenticate the complete DNORST02
proof before finishing the explicitly selected filesystem transaction. That
closure is an as-of-intent header view, not proof of globally latest work and
not live state/owner authority. Dedicated startup must still merge/recheck
current stored header branches before enrolling any compact owner. Missing,
corrupt, wrong-work or conflicting header data refuses without repairing the
original store. Native tests discard proof/selector inputs in the same process;
they do not establish fresh-process crash recovery or phone/App acceptance.

The selected activation-parent route now has a private existing-layout writable
open and a distinct typed state-consistency owner. It requires the genuine closed
lifecycle receipt and retained root, completed exact filesystem selection, current
branch-preserving HeaderStore selector, complete DNORST02/burial verification,
all original rows/before-images and the exact candidate logical baseline. The
retained original is rebound read-only under its backup name before opening the
new live writer: process lock registries must not retain the old live pathname.
No create, schema append, migration, lock retry or generic startup-fence bypass
is exposed. Once a completed baseline has been privately opened, LOG rotations
may change physical file metadata; cold parent reopening still checks the entire
recorded logical baseline, original immutable inventory, layout and inner paths.
This parent-only route refuses mutated descendant/post-undo baselines until their
dedicated full suffix startup audit is implemented; it never guesses a repair.

The typed consistency owner pins the actual database open generation, writer
mutex, full existing validation fingerprint, origin/selection and original
independent-history anchor. Compact connect, disconnect, retirement staging,
pool input capture and pre-commit readiness use this SAME authority. Real tip,
forest, active height, frozen legacy, authorizations, sets, body/index/undo,
catalog and delivery checks remain mandatory. The validated marker is neither
advanced nor rebased by this route. Missing/corrupt/changed origin or anchor,
foreign/reopened engine or profile changes refuse; a decoded row cannot mint
the type. Historical pre-activation writers may not consume it. Default full/
independent staging retains its previous validated-tip rule. The shared profile
and error definitions were extracted without changing their token sequence.
Native tests compose an empty-genesis named-wallet lifecycle receipt with an
offline authenticated parent, execute first/second compact connects and undo,
and exercise cold parent open/refusals. This is not automatic App/SDK activation,
queued-payment survival, descendant cold startup, complete phone history,
fresh-process crash, physical device or release qualification.

The private suffix DATA audit now revalidates every selected mixed body from
the authenticated activation proof, including the first block's authorizations,
state and catalog. A scratch byte oracle stages only those verified Orchard and
retirement transitions: it has no tip, validated marker, forest, independent
replay capability or publication owner. Entire `O1` and `R1` namespaces are
compared, including nullifier **owners**, anchor references and undo, rather than
only committed set digests. The audit also checks exact embedded/archive bodies,
undo locators, deltas, filters, transaction indexes, commit journals, reachable
catalog nodes, full frozen legacy contents and local outbox framing/continuity.
Checksummed node iteration into private MAC disk staging and a disk-backed seen
set avoid per-node source-SST reads and an unbounded resident tree walk. Batches,
free-space and cancellation checks are not a production latency/RSS guarantee.
No total 2,048-event or 64-MiB historical replay ceiling is reinstated.

Existing parent handoff opening consumes this audit while retaining all its
original immutable source/cohort/name/receipt/before-image checks. Same-process
tests additionally close/reopen an actual descendant or undone-parent database
read-only and exercise wrong-owner, orphan-row, bad-delta, missing-body and
cancellation refusals without repair or writer publication. The DATA return is
not completed-root/current-before-image or lifecycle authority. Descendant and
post-undo **writer/DaemonApp/SDK startup** still require their dedicated routing
and full original/current before-image audit. Local delivery hash-chain checks
alone are not adversarial authentication or a first-runtime-event certificate.
No actual queued-wallet crossing, restored-phone or device acceptance is implied.

The parent opener additionally authenticates the delivery suffix from the actual
immutable original store's outbox head. A private disk-backed replay validates
each genuine connect and checks disconnect against checkpoints captured only by
those validations, including exact bodies, Orchard state, conventional undo and
branch MTP answers. Immutable anchor/nullifier occurrences are filtered by the
replayed branch ancestry, so undo/reconnect does not accumulate stale members.
Owned content-addressed checkpoint roots bind metadata independently of scratch
MAC records; decoded spool material cannot construct a sealed validation result.
The final selected state/catalog must match the complete replay. A locally
reframed event checksum is not enough. No first cursor or independent anchor is
fabricated, no canonical rows are repaired, and the replay returns DATA only.
Actual close/read-only-reopen and local-checksum-valid false-MTP cases are
component controls, not normal descendant/post-undo writer/DaemonApp startup.
That route still requires an exact full original/current row audit, including
retained records from undone branches, before a writable owner can be published.

The unpublished conversion now uses one private deterministic parent-copy builder,
also suitable for reconstructing the exact selected before-image during cold
auditing. It creates its own exclusive destination, pins its inode before opening
an engine, checksums and copies every original column-family row in bounded synced
batches, parses every removed coin, and stages the same proof/header/catalog/origin
encodings. The actual root/source fence still runs before the final synced choice.
It cannot reuse or clear a caller-supplied engine. Tests reconstruct through cold
retained headers and compare every row, retain unknown original metadata, reject a
wrong source digest or cancellation before creation, and preserve missing choice
rows if the final fence refuses. This builder is DATA copying, not a state owner,
normal descendant startup, lifecycle recovery or permission to replay a capture.

The private handoff opener now also reconstructs the exact converted parent
from the immutable original and retained as-of-intent headers. Its whole-row
digest must match the recorded selection. Authenticated actual delivery events
then drive the same canonical compact connect/undo writer in an exclusive
scratch engine. The scratch policy can read only retained body/undo archives;
missing locators refuse instead of appending to the real archives. A scratch
selected-consistency owner is private, bound to that engine, never published and
never presented as independent execution of pre-activation history.

The expected engine retains canonical rows from undone blocks as well as the
active branch. Every CF name/key/value is compared, including unknown original
metadata and non-Orchard namespaces. Only the canonical tip receipt's wall-clock
timestamp is normalized, after exact parsing, canonical re-encoding and matching
hash/height/work. No mutable prefix is exempt. This is required by the existing
parent handoff opener; normal descendant/post-undo daemon startup and SDK/phone
acceptance still require their separate completed-root and service routing.

Normal completed-root startup is now a separate path from unfinished PUBLIC
reopen-intent recovery. The same real datadir guard pins the recorded completed
choice and retained original directory/cohort. Stores open read-only first;
current HeaderStore branches and retained raw headers are independently checked
before the complete before-image/suffix audits. Only then may an existing-layout
writer reopen the same selected DB, recheck logical rows and source/root identity,
and bind a new generation/mutex-specific selected-origin consistency owner.
Normal startup does not fabricate a closed lifecycle receipt or replay a stale
capture. A still-pending wallet/pool intent is recovered separately by its host.

The service preserves original prehistory flags and validated anchor instead of
passing them through the independent-replay equality branch. Its header/index,
boundary, inverse proof and delivery admission use the distinct bound capability;
delivery starts from the actual immutable original head, including nonzero or
empty heads, never a fabricated first-block cursor. Existing full/fresh/snapshot
and independent-replay guards remain strict for their original owner types.
An ordinary closed node's canonical persisted pruning policy must be present in
the before-image. Missing/noncanonical policy refuses before writable startup;
it is not seeded during restart or exempted from whole-row comparison.

The real-process regression uses fixed test-only parameters and fixture-host
conversion, then actual production DaemonApp/ChainstateService startup in fresh
processes. It covers descendant, post-undo and activation-parent states, two cold
starts each, unchanged original stores/anchor and normal named-wallet opening
without a phrase. Added-row, changed-original, advanced-anchor, aliased-control
and absent-policy roots refuse. The queued corpus is empty: this does NOT satisfy
the real named-wallet/queued-payment crossing, restored-wallet earlier-funds,
original mobile assumed-history lifecycle, SDK worker or physical-device gates.
Existing strict wallet-index assumed-history checks are not erased to pass it.

Mutation controls are recorded separately from the earlier positive receipts.
The original parent-reopen case survived a disabled consistency check; transition
and engine-reopen cases detected it. The parent case now additionally substitutes
a validly encoded, paired foreign origin and advances the independent anchor,
requiring refusal without writes and successful capture again after each test
restoration. Isolated origin/anchor, generation and disabled-check mutants must
be paired with the unchanged/restored check before claiming mutation coverage.
These test-only substitutions never change release guard code or installed data.

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

### Embedded catalog staging root regression

The catalog download host must use `ConfigService::DataDir()`, which is bound to
the operator root by normal startup. The legacy `GetConfig().datadir` may be
empty in the embedded process; an empty path prevents the download actor from
starting even with eligible peers and locally selected activation headers.
This path selection does not authorize a store or change transfer verification.

`NodeCoreActivationWire` compiles the actual SDK worker with a fixed, native-test-
only private regtest profile selection. It drives a real loopback peer, nonce-
bound catalog pages, the SDK's own orderly handoff, normal replacement startup
and named-wallet reopening without a phrase. Its exact pre-Orchard source comes
from independent execution with genuine historical undo/delta/checkpoints; the
visited proof-producing fixture is preserved separately. No lifecycle receipt,
ready proof, first event, replay authority or accepted owner is injected.
The test also observes no reply from queue RPCs during the real pause. After
publication, the untrusted peer serves the requested full `MSG_BLOCK` bodies
for activation and its descendant. The actual queue validates and connects
both; no first runtime event or cursor is injected. A newly restored named
wallet then uses the public account-0 recovery RPC to find the earlier private
funds and exact 512-byte memo, remains `generated=false`, encrypts and passes a
normal SDK cold reopen and wallet open/unlock without re-entering the phrase.

This is a short activation-origin download into retained authenticated history,
not a fresh late phone at a post-activation snapshot floor. Original mobile
assumed-history/SQL readiness, detached activation-to-tip historical backfill
for that late phone, nonempty queued-payment crossing, physical-device and
release acceptance remain separate gates. The original parent-only receipts
remain scoped to their original source and are not relabelled by this test.

The first actual full-body SDK cold-reopen regression also caught a missing
proof lifetime lookup: a single compact extension retained its genuine bound
validation, but transition completion could only find the old-tip inverse or
the multi-block branch proof. Its final audit consequently deferred, leaving
the activation recovery marker behind despite the committed tip. Completion
now exposes that same immutable extension proof to the existing strict inverse
audit, with source/database/storage/thread/profile/body/selected-tip binding.
No parent rebind is attempted after commit, no audit is skipped, and no startup
marker is deleted as a workaround. Failed input roots and their marker remain
preserved separately.
The first-block extension also captures canonical empty logical-set hashes from
the checked empty separated namespace, not a zero-initialized digest struct.
The shared reader refuses existing state or any untracked Orchard namespace
row; this does not certify wallet history or independently executed prehistory.
