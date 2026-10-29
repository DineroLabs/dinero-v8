# Account recovery and authenticated operation-archive inventory

## Problem and behavior

Completed operations use `OrchardOperationArchive` records stored in the same
`orchard_wallet_snapshots` table as accounts. Their wallet IDs are derived from
the owning wallet and operation ID. Treating every distinct ID as a foreign
account prevented enrolled-wallet recovery as soon as an operation was archived.

`OrchardAccountDelivery::ReadEnrolledForReplay` now captures the complete row
inventory in its existing owned FULL SQLite transaction. It restores every
current account under the selected wallet's keys and persistent identity, then
follows that account's authenticated archive head and predecessor links through
the existing archive reader. Every reached record must authenticate, decode,
match its exact derived identity/account/revision, and occur only once. Every
captured row must be accounted for by an authenticated account or one of these
reached records. Unrelated foreign rows, missing records and corrupted envelopes
still refuse recovery. Rows are never silently filtered out.

The existing coordinator compares reached archive identities/revisions as well
as account revisions/cursors before each source event and after the final source
read. A changed archive cannot be hidden by an unchanged account delivery cursor.
The archive itself remains unchanged by these reads. Existing ordered partial
commits and authenticated parent rollback are retained. The actual index-delivery
component now declares its operation-archive dependency, including for GNU ld.

The inventory has an operational ceiling of 65,536 snapshot rows and the existing
1,024-account ceiling. Excess inventory refuses before effects; neither ceiling
truncates records or certifies memory/load behavior. Archive traversal uses the
existing 64-record pages and finishes all referenced records before returning.

## Scope

This authenticates archive membership relative to each present authenticated
account head. It does not create an authenticated catalog proving that no account
was deleted before discovery, or detect a consistent rollback of an entire wallet
backup. Existing old account-format/archive compatibility and explicit migration
must satisfy the archive reader's current-state check; there is no silent rewrite.

This is inventory validation in the real recovery owner. It is not automatic
reactivation of archived operations after their recorded cause disconnects, a
baseline certificate, or readiness for selection/broadcast. That reconciliation,
pre-origin/late-account/rescan handling, general long-history operation, remaining
consumers, production installation and activation/lifecycle qualification remain
open. The explicit-one-account recovery API retains its narrower scope.

## Qualification

The new integration fixture creates a genuine pending shield intent using actual
resolved transparent inputs, observes its conflict in the checked source block,
and archives it through `StageCompleted` in the real wallet database. The original
delivery implementation rejected that record before the production change. The
first fixture link required declaring the existing archive dependency; the link
failure was not counted as the intended regression.

The actual declared integration target checks archive-aware enrolled recovery,
missing/corrupt referenced records, unrelated foreign rows, authenticated archive
revision changes during source acquisition, source replay/undo/reconnect and real
wallet reopen. Existing account and archive suites cover their own storage and
fresh-process cases. Generated bodies and explicitly enrolled baselines are not
independently validated historical consensus or a running-node recovery test.

Fresh declared CMake targets and the full daemon build pass. The final three
local CTests pass: `OrchardIndexDelivery` (55.85 seconds),
`OrchardOperationArchive` (8.42 seconds), and `OrchardAccountDelivery` (17.93
seconds). The actual service translation unit also compiles with
`DINERO_HAS_ORCHARD_RUNTIME_READER` removed. The unchanged Orchard workflow's
inventory and execution selectors each contain 46 enabled registrations; only
the three affected lanes ran locally.

All 122 linked project C++ translation units in the integration executable were
freshly instrumented with ASan/UBSan. Final link maps exclude project C++ archive
members. Rust/external libraries remain uninstrumented and macOS LSan is off;
the public service adapter is outside that executable's sanitizer scope. The
separate ARM RocksDB gate remains open. Copied-source controls exercise the
original account-only discovery, omission of the unclaimed-row check and omission
of the archive-revision recheck. All three failed the intended regression
without sanitizer diagnostics, and the restored executable passed again.

The archive-revision test initially queried a temporary table after wallet
reopen, then attempted a second recovery-seed owner while one was pinned. The
fixture now obtains the authenticated archive locator from the actual owner
before copying the seed. Both refusals were preserved; production ownership
checks and test expectations were not weakened. No new physical-power-loss,
whole-node recovery, general load or release-binary provenance claim is made.
Local build labels and prebuilt dependencies are inherited from the RPC repair.
