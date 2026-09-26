# Owned Orchard chainstate commit

## Purpose and scope

`PreparedOrchardChainstateWrite` owns the activation lock, private RocksDB batch
and prepared memory publication for one stateful connect or disconnect. A caller
cannot append records to its batch or publish its coins before durability. This
closes the mutable-batch boundary of the existing full staging adapter.

This is a daemon ownership component, not production block admission. The
historical `ConnectTip`/`DisconnectTip` routes remain unchanged. Before using it
there, the service must complete selected header/PoW validation, flatfile and
block-index coordination (now available through the indexed variants),
active-chain metadata publication and notifications.
CSN authentication, startup/replay/reindex routing, ordinary confidential
transaction compatibility and wallet SQLite coordination remain separate work.
All shipped Orchard activation switches remain unset.

## Lifetime and failure rules

The factories acquire the supplied activation mutex before staging any reads or
writes. The service must supply its real writer mutex and prevent every other
writer from bypassing it. Database and write-token lifetimes must cover the
object's lifetime. The boundary retirement record retains the existing selected,
validated-history requirement; the owner does not certify history by taking a
lock or accepting that record.

Preparation runs the full stateful staging adapter, then preallocates memory
publication from that exact transition. The private batch contains the combined
coin, Orchard, forest, undo, body, active transaction indexes, filter, journal,
retirement and tip records. There is no batch accessor, append API, separate
publication API or move constructor.

`Commit()` rechecks the memory source, writes the whole batch synchronously once,
then publishes the preallocated coin/forest/tip view. The lock remains held until
destruction, including after commit, so the service can complete its own prepared
metadata publication under the same lock. No reentrant database or memory writes
are permitted while the object exists. A recursive mutex excludes other threads;
it is not a general firewall against unrelated writes by its owning thread.

- Abandonment or preparation failure performs no database write and releases the
  lock. Memory capacity can have been reserved; logical coins and tip are unchanged.
- A readiness failure invalidates the object before any database write. Restoring
  the source state does not make that object retryable.
- Wrong-thread use and duplicate commit are rejected. Destruction on the wrong
  thread terminates before attempting an invalid mutex unlock.
- Once the write starts, a returned storage error or exception terminates the
  process. The node must recover from durable state on restart rather than carry
  on with potentially divergent memory. This does not change ChainDB's existing
  internal bounded retry behavior.
- Memory publication remains non-throwing and fail-stop on an invariant breach.

## Regression coverage

`OrchardCommitOwner` uses the existing generated full-stateful fixture: real
Orchard authorization, transparent same-block spending, Utreexo proof, compact
filter and DNRS commitment. It checks both checkpoint settings, full database /
memory agreement, abandonment in both directions, competing-thread lock
exclusion, wrong-thread commit rejection, stale-memory abort and duplicate use.

Fresh subprocesses exit before commit and after publication in both directions,
then the parent compares all consensus rows and restores the forest. Additional
subprocesses close their temporary database after preparation to force a storage
error: returning to the caller is a test failure. This exercises the owner's
fail-stop decision, not torn sectors, filesystem durability or every RocksDB I/O
error. Existing low-level tests retain the post-durable/pre-publication boundary.

All stores, keys and amounts are synthetic. This is not a mainnet migration,
whole-daemon lifecycle result, shutdown-budget result or release-binary receipt.
