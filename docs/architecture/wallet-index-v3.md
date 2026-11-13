# Wallet index v3: one authenticated namespace per wallet

## Why this is needed

The full-node `blockchain/utxo` database contains one `wallet_utxos` table and
one runtime delivery receipt. Its key is `(txid, vout)`, with no wallet
identity. An isolated named-wallet upgrade replayed the canonical source but
could not reconcile: the first existing index row was from a different named
wallet. Reassigning or deleting that row would be unsafe. The existing table
must remain evidence until each wallet's own source has been checked.

## Chosen boundary

Keep the chainstate metadata and the v2 rows in the same SQLite file. Before
any version bump, create a WAL-consistent SQLite backup of the entire v2 index
with `sqlite3_backup` into a same-directory temporary file. Verify the backup
as version 2 with `quick_check`, row/value digest, and chainstate metadata
digest equal to the source; fsync it, publish it under an immutable
content-digest name without replacing an older backup, and fsync the parent
directory. If any step fails, refuse the migration. A crash after backup
publication may be followed by a legitimate v2 write: a retry then makes a
new backup under a new digest name, retaining the first. The chosen backup
digest is committed with version 3 in `utxo_metadata` and verified on every
v3 reopen. Test that the old v8.2.2 `UTXOIndex` can open this exact backup.
Only then, in one `BEGIN IMMEDIATE` transaction at startup, after verifying
the exact v2 schema:

1. Rename `wallet_utxos` to `wallet_utxos_legacy_v2`, preserving all rows and
   value types. Record its count and a deterministic row digest, and verify
   them before commit. No v2 row is assigned to a wallet by script/path/name.
2. Create a new `wallet_utxos` table with a required, opaque wallet delivery
   identity and primary key `(wallet_identity, txid, vout)`. Leave it empty.
   The renamed v2 table retains its old indexes/triggers as quarantined
   evidence; the new table gets fresh v3 indexes and per-wallet receipt
   invalidation triggers with distinct names. No old trigger may target v3.
3. Set `PRAGMA user_version=3`, record the selected immutable backup digest,
   and commit at `synchronous=FULL`. Existing
   v8.2.2 code refuses versions above 2, so an old binary cannot reinterpret
   the migrated database. A database backup is still required for rollback.

All wallet coin reads/writes, including direct SQL outside `UTXOIndex`, must
bind the selected wallet's exact delivery identity. An absent identity or
unqualified receipt means unavailable, never an unscoped query or zero balance.
Per-wallet receipts and invalidation live in a v3 table keyed by that identity;
the old global `runtime_delivery:v1:receipt` and invalidation keys remain
quarantined as v2 evidence, with no fallback into v3 read/write decisions.
The full-node chainstate
keys in `utxo_metadata` do not move.

Legacy transparent recovery first proves the source and wallet-owned
coin/history versions, then writes only its v3 namespace and receipt. It
commits its wallet ordinary receipt second. Pending stays fail closed between
those commits; restart resumes from the exact identity/source prefix. A second
wallet cannot reuse another wallet's receipt. Switching wallets changes only
the active identity under the wallet session and index lock; it never swaps or relabels
coin rows. Raw `UTXOIndex` readers require a session-bound identity token.
Financial reads also require the wallet SQL tip, `sync_meta` scan height,
per-wallet ordinary and index receipts, selected chain tip, and in-memory
wallet height to agree. The in-memory height is published only after both
receipts commit; a current receipt with stale memory is not sufficient because
coinbase maturity uses the in-memory height.

The schema upgrade is an explicit full-archival chainstate operation, not a
change to generic `UTXOIndex::Initialize()`: compact/NodeCore phone stores stay
on their existing v2 format and current recovery path. The index API must
distinguish v2 compact from v3 full archive, refusing any mismatched mode or
attempt to open v3 through an older v2-only caller. The existing phone
selected-history 128 MiB and typed-tail budgets are unrelated and unchanged.

## Full-archive startup order (activation gate)

The v3 upgrade must be called only after `DatadirGuard` holds the resolved
datadir lock and before any `UTXOIndex`, wallet worker, or runtime delivery
instance opens `blockchain/utxo`. `DaemonApp` owns that guard; the upgrade
belongs before its service initialization loop, where `ChainstateService`
currently opens the index in `InitStorage`. Probe the index version on a
read-only connection while the guard is held: v2 takes the backup and upgrade
path, v3 takes verification only, and every other version refuses. A failed
backup or schema transaction stops startup before wallet services run. The
generic `UTXOIndex::Initialize()` continues to refuse v3.

After v3 verification, the full-archive chainstate can use
`InitializeFullArchiveV3MetadataOnly()` to access chainstate keys in
`utxo_metadata`. That method opens and checks the *same* SQLite handle that
serves later metadata reads. It explicitly refuses all v2 coin APIs and old
global runtime receipts; it does not make wallets ready. Compact stores retain
v2 `Initialize()`. The current full-node `VerifyUTXOSetMatch` compares the
old global wallet-index count with snapshot metadata. A v3 wallet namespace
cannot stand in for that count; move this check to a consensus-owned coin
source or refuse it when the old metadata key is present. The chainwide fatal
reset must call the v3 reset transaction, not `UTXOIndex::ClearAll()`.

Only after owner-scoped typed index delivery, per-wallet receipts, the
ordinary-wallet receipt, and selected-tip/memory-height fences are qualified
may the startup upgrade be wired. In particular, no source path may call
legacy `ValidateAgainstConsensus`, `ScanBlockIdempotent`, `WalletUTXOAdapter`,
or a raw `wallet_utxos` query after opening v3. A wallet without a durable
delivery ID first enters the explicit wallet-owned migration transaction that
commits ID and Pending together; ordinary index reads never mint an ID.

The initial schema slice implemented the owner token, raw read-only v3 coin
view, and metadata-only open without a production startup hook or financial
balance path. An isolated full-node index with mixed legacy rows passed a
read-only-source to scratch-copy migration/reopen test at that stage. Later
recovery and financial-read changes have their own qualification gates.

The backup identity includes exact v2 coin rows, chainstate metadata, and
index/trigger SQL on both legacy tables. A guard-only edit after an earlier
backup therefore gets a different immutable backup. The upgrade separately
seals the exact SQL of the renamed legacy guards and checks it on every v3
reopen. The v3 metadata-only handle opens the UTF-8 path with NOFOLLOW and
without CREATE, verifies that same handle before PRAGMAs, and treats SQL
read failures as errors rather than absent recovery markers.

## Owner-scoped delivery order (next implementation slice)

The existing `RuntimeIndexDelivery` methods are v2-only. They read the single
`runtime_delivery:v1:receipt`, scan every `wallet_utxos` row in
`CoverageIndexRows`, and issue SQL mutations by `(txid,vout)` alone. They
explicitly refuse a metadata-only v3 index. They must not be repurposed by
merely adding an identity column to the table.

The v3 writer needs a distinct path using the checked `RuntimeWalletCoverageProjection`:

The index-only writer `RuntimeIndexDelivery::ReconcileCoverageV3` requires a
metadata-only v3 index, a live Pending wallet lease, an existing durable
identity, a complete checked full-archive projection, and a source callback
that calls COMMIT while holding the selected-source lock. Its SQL uses the
owner token on every row and receipt statement. It deliberately does not mark
the wallet Complete or expose a financial balance.

`RuntimeOrdinaryDelivery::ReconcileLegacyTransparentCoverageV3` now commits
the selected wallet's ordinary rows and scan height after checking that exact
v3 index receipt in a separate transaction. It seals the ordinary receipt and
Complete marker only under a second source check. An injected failure between
the two commits leaves Pending; closing and reopening the v3 metadata handle
then retries from the proved index receipt. The chainstate caller must hold
its activation lock before either wallet or index ownership and retain it
through both commits, preserving the established lock order. This writer is
still not routed from startup. `RuntimeIndexDelivery::ApplyForWalletV3` also
applies one later checked typed block under that wallet's identity. Its
index-first commit is restartable: if ordinary delivery has not committed,
an exact replay rechecks the v3 receipt and lets ordinary delivery catch up.
An omitted source commit rolls back the row effects. Owner-scoped financial
queries, a complete runtime coordinator, and startup routing remain
unimplemented. The financial RPC entry now has a dormant v3 receipt fence:
when the full-archive metadata index is installed, it checks the selected
wallet's exact v3 index receipt and ordinary receipt against the selected tip
under the wallet lease before returning numbers. A foreign, stale, missing, or
invalidated v3 receipt refuses the read. The tests exercise this narrow check
and an actual absent-wallet financial RPC, not complete v3 startup. V3
activation must stay disabled until those routes and the full clone tests are
qualified.

An unenrolled wallet with an absent migration marker now refuses financial RPC
reads as soon as a v3 metadata index is selected, even when its old SQL scan
height matches the selected tip. The v3 receipt reader uses the metadata
index's already verified SQLite connection under its mutex and a read
transaction. It rechecks exact table and guard definitions plus the owner's
receipt on that transaction. The handle pins the rollback backup digest and
legacy guard digest authenticated at open; each read compares both durable
markers to those pinned values. A well-formed replacement backup digest or a
correlated legacy guard/marker edit therefore refuses financial reads until
the handle is reopened and fully verified. Full backup/quarantine content
verification remains an open/reopen gate, not a per-balance scan.

1. Acquire the wallet database lease and capture the noncopyable v3 owner
   token. A never-enrolled wallet must have entered the deliberate atomic
   ID+Pending transaction first; an ordinary index read cannot create the ID.
   Recheck the source's session, existing identity, and exact durable script
   domain under that lease. Capture the origin without passing the old v2
   index domain; the quarantined v2 rows are evidence, not ownership.
2. Under wallet lease then v3 index transaction, read only
   `wallet_index_receipts_v3 WHERE wallet_identity=?`. Decode and check a
   receipt against the selected source prefix, ownership digest and profile.
   A missing, invalidated, or foreign receipt never inherits the global v1
   receipt. A retry after an index-only commit rechecks that exact prefix.
3. Query existing coins only with `WHERE wallet_identity=?`, validate each
   against the source's observed versions, and apply INSERT/UPDATE/DELETE
   with the same identity bound in every statement. The selected wallet can
   receive an outpoint also owned by another wallet without modifying the
   other row or its receipt. The new receipt is sealed after all row effects
   in the index transaction; a source-head fence runs before commit.
4. Reconcile ordinary wallet coins/history and scan height in its own checked
   transaction, then seal its ordinary receipt and mark transparent recovery
   complete. Pending remains fail-closed between the two commits. Publish
   in-memory wallet height only after both receipts and selected-tip checks.
   Later typed events need the same owner token and SQL predicates.

Regression gates: two identities with the same outpoint and different
histories; B spends while A is inactive, then A refuses until its own replay;
crash after index receipt before ordinary receipt and resume; wallet switch
while a worker has an A handle; absent identity; foreign/global v1 receipt;
corrupt v3 row/body/source; receipt invalidation by an out-of-band mutation;
and a full process restart with exact balances and maturity. Keep the v2
delivery path intact for compact phones.

## Qualification before rollout

- Migrate a v2 clone with mixed named-wallet and unknown rows. Verify every
  legacy row value and all chainstate metadata are unchanged after v3 commit.
  Also test backup failure, WAL-resident writes, crash before rename, and crash
  after backup but before the schema commit; none may publish a partial v3.
- Kill before/after the schema commit, after index receipt, after wallet
  receipt, and during a wallet switch. Restart must either retain the v2 DB or
  resume a precise v3 Pending state, never show another wallet's balance.
- The old v8.2.2 binary must refuse the migrated clone before wallet use.
- Rebuild two different wallets from a full source, including the same
  outpoint owned by two wallet identities. Each gets its own history, balance,
  maturity, receipt, and restart state; no cross-wallet credit or send.
- Corrupt/missing body, mismatched source tip, unmatched old wallet row, and
  receipt owner mismatch must refuse without advancing a watermark.
- Audit every product SQL reference to `wallet_utxos` and test that no SELECT,
  UPDATE, INSERT, or DELETE can run without an exact wallet identity.
- Keep compact-v6 phone v2 behavior green with Orchard ON/OFF and qualify the
  full-archive v3 build on Linux, macOS, and Windows. Qualify unchanged v2
  compact storage on iOS and Android before any NodeCore SDK rebuild or deployment.
- On an isolated data copy, verify the canonical tip, named-wallet scan
  tip, mined history timestamps, balance, limited send state, restart, and
  unchanged quarantine count/digest. Only then consider a live pilot.

This is a separate source lane. No v3 schema or live wallet mutation is part of
the current combined 8.2.3 candidate.
