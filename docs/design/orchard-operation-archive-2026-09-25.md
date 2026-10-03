# Orchard durable operation history

`OrchardOperationArchive` completes the storage boundary for moving confirmed or
conflicted operations out of the bounded pending queue. It is an optional wallet
component; daemon wallet RPCs and chain notifications do not invoke it yet.

## Atomic movement

The caller holds wallet and selected-chain locks and begins a durable SQLite
transaction on the existing wallet connection. `StageCompleted` rechecks the
account revision and bytes and authenticates the selected block supporting the
observation. It stages the encrypted history record and updated account together.
The caller stages ordinary wallet reservation changes in that same transaction,
commits, and only then publishes the returned account/revision. Success from the
staging function is not durable success or permission to relay.

A savepoint rolls back both internal writes if either fails, including when the
caller catches the exception and commits its outer transaction. An unrecoverable
savepoint rollback/release error terminates the process if SQLite still has an
active outer transaction: continuing could permit a partial move to commit. If
SQLite has already rolled back the whole transaction, the original error
propagates. Normal operation does not commit or roll back the outer transaction.

Each record uses the existing snapshot AEAD, revision checks and seed-derived
key scheme under a domain-separated identity derived from wallet ID and operation
ID. Plain operation IDs are not stored in a separate history index. The in-memory
seed and serialized plaintext buffers use the existing wiping containers; this
is not a guarantee against swap, caller copies or a compromised process.

## Authenticated bounded history

Account format `DNORAC03` adds an encrypted archive count/head checkpoint. Readers
retain compatibility with staged `DNORAC01` and `DNORAC02` account snapshots.
Each `DNORAR01` record contains its sequence, predecessor ID, one complete
operation, and its confirmation/conflict observation. Links and sequence cannot
change when an operation is archived again. History is retained; there is no
prune/delete API.

`Begin` authenticates the current account and head. `List` follows at most 64
predecessor records per call and checks descending sequence and the terminal
link. Missing or damaged records fail rather than silently shorten history.
Whole-history completeness is established only after reaching the end, not by
checking the head alone. Storage faults and lookup errors must stop recovery.
This detects missing linked records; it does not prevent an attacker restoring
an entire older, internally consistent wallet/database backup.

The cursor and returned operation tickets have private constructors and carry
the captured root checkpoint. The host retains its wallet/chain snapshot while
walking the pages. Reactivation accepts tickets from this authenticated traversal,
not an arbitrary operation ID. Unrelated record reads do not establish membership
in the selected account's archive.

## Reorg and interrupted proving

Reactivation requires the scanner's selected checkpoint and evidence that the
recorded confirmation/conflict block is disconnected. Failed selected-hash
lookups are errors, never proof of a reorg. The original operation and reservations
return to pending; capacity or reservation conflicts leave the archive intact.
Ready bytes are preserved and still require fresh node admission before relay.
The archive remains available after reactivation.

A Reserved operation can be archived on a conflict. After a reorg, the same
intent may become Ready, then be archived again under its original sequence.
The signed message, prevouts and nullifiers must remain identical. Ready bytes
cannot change or regress to Reserved. After process restart the randomized
proving plan is unavailable, so the wallet must cancel/re-reserve an unfinished
intent through the normal durable protocol instead of reconstructing a different
plan under its old signing message.

The runtime must reconcile history before selecting new inputs. More than 128
operations may need reconsideration after a deep reorg; the bounded pending queue
must not cause history deletion. A service backlog, readmission, broadcast and
selection readiness policy remain integration work. These component APIs do not
claim that the live wallet already enforces that policy.

## Executed qualification

The component uses fresh Orchard proofs and native transparent signatures on
synthetic regtest data. Tests cover archive commit/rollback, an injected account
write failure followed by outer commit, encrypted close/reopen, authenticated
pagination, a missing predecessor, selected-chain errors, exact-byte resurrection,
reconfirmation without duplicate history, and Reserved-to-Ready continuation.
Fresh subprocesses exit before and after archive/account/companion-state commit.
The account reader additionally restores old staged formats 01 and 02.

The root workflow builds and explicitly requires `OrchardOperationArchive`;
this is not an existence-conditional or label-only registration. Full daemon
compilation and all selected tests are required alongside this component. Local
qualification is distinct from final-source Linux and release-binary provenance.
